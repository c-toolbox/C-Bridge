/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Audio smoke test for the Opus path used by both sinks.
//
// 1. NDI path: feed known-tone Opus packets (loaded from a reference .ogg file, or encoded
//    in-process when no file is given) through the production AudioDecoder and verify that
//    the decoded planar float frames match the original waveform per channel.
// 2. TS path: mux the same packets into an MPEG-TS file with exactly the parameters
//    TsMulticastSink uses, demux it again, decode it, and re-verify - proving that the
//    multicast output is playable audio.
// 3. Sanitizer: structural checks for OpusPayloadSanitizer plus a TS round trip of multi-frame
//    tone payloads, proving the container path carries two-frames-per-packet Opus intact. The live
//    stream's specific defect (mode-1 CBR pairs with one undeclared trailing byte) is verified
//    against real dumps: audio-smoke-test.exe <dump>.raw sanitizes every packet, decodes the result
//    with the production decoder, and writes a playable <name>_sanitized.ts next to it.
//
// Build and run:
//   cmake --build build --target audio-smoke-test --config Release
//   audio-smoke-test.exe [stereo_ref.ogg] [mono_ref.ogg]
//   audio-smoke-test.exe capture/webrtc_audio_packets.raw  (offline sanitizer analysis of a dump)

#include "media/audiodecoder.h"
#include "media/avwrappers.h"
#include "media/opuspayloadsanitizer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <QDir>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <numbers>
#include <vector>

using namespace Qt::Literals::StringLiterals;

// The production build gets this from cudacontext.cpp, which the smoke test does not link, so
// provide its own copy of the shared helper declared in avwrappers.h.
namespace CBridge {

QString avErrorString(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

} // namespace CBridge

namespace {

constexpr int kSampleRate = 48000;
constexpr int kDurationSeconds = 2;
constexpr int kFrameSize = 960; // 20 ms at 48 kHz, a valid Opus frame size
constexpr double kAmplitude = 0.5;

// Different tone per channel so a swap or mix-up in the planar conversion shows up as a
// failed correlation instead of passing silently.
double toneFrequency(int channel) { return channel == 0 ? 440.0 : 550.0; }

struct DecodedAudio
{
    std::vector<float> channels[2]; // planar, one vector per channel
    int sampleRate = 0;
    int channelCount = 0;
    bool formatOk = true;
    long frameCount = 0;
};

// Collects decoded frames the same way NdiSink::onDecodedAudio does.
void collectFrames(AVFrame *frame, DecodedAudio &out)
{
    if (frame->format != AV_SAMPLE_FMT_FLTP || frame->nb_samples <= 0) {
        out.formatOk = false;
        return;
    }
    const int channels = frame->ch_layout.nb_channels;
    if (channels < 1 || channels > 2) {
        out.formatOk = false;
        return;
    }
    if (out.channelCount == 0) {
        out.sampleRate = frame->sample_rate;
        out.channelCount = channels;
    } else if (frame->sample_rate != out.sampleRate || channels != out.channelCount) {
        out.formatOk = false;
        return;
    }

    for (int c = 0; c < channels; ++c) {
        const float *plane = reinterpret_cast<const float *>(frame->data[c]);
        auto &outPlane = out.channels[c];
        outPlane.insert(outPlane.end(), plane, plane + frame->nb_samples);
    }
    ++out.frameCount;
}

// Fills a frame with the per-channel reference tones.
void fillToneFrame(AVFrame *frame, std::size_t sampleOffset)
{
    const int channels = frame->ch_layout.nb_channels;
    for (int i = 0; i < frame->nb_samples; ++i) {
        const double t = double(sampleOffset + std::size_t(i)) / kSampleRate;
        if (frame->format == AV_SAMPLE_FMT_FLTP) {
            for (int c = 0; c < channels; ++c) {
                float *plane = reinterpret_cast<float *>(frame->data[c]);
                plane[i] = static_cast<float>(kAmplitude * std::sin(2.0 * std::numbers::pi
                                                                    * toneFrequency(c) * t));
            }
        } else if (frame->format == AV_SAMPLE_FMT_S16P) {
            for (int c = 0; c < channels; ++c) {
                auto *plane = reinterpret_cast<std::int16_t *>(frame->data[c]);
                plane[i] = static_cast<std::int16_t>(kAmplitude * 32767.0 * std::sin(
                    2.0 * std::numbers::pi * toneFrequency(c) * t));
            }
        } else { // packed/interleaved formats: float or integer elements
            const int bytesPerSample = av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format));
            for (int c = 0; c < channels; ++c) {
                const double value = kAmplitude * std::sin(2.0 * std::numbers::pi
                                                           * toneFrequency(c) * t);
                uint8_t *slot = frame->data[0] + (std::size_t(i) * channels + c) * bytesPerSample;
                if (frame->format == AV_SAMPLE_FMT_FLT) {
                    const float f = static_cast<float>(value);
                    std::memcpy(slot, &f, sizeof(f));
                } else if (bytesPerSample >= 2) {
                    const std::int16_t s = static_cast<std::int16_t>(value * 32767.0);
                    std::memcpy(slot, &s, sizeof(s));
                }
            }
        }
    }
}

// Drives an opened encoder with the reference tone and collects its packets.
bool driveEncoder(CBridge::CodecContextPtr &context, std::vector<std::vector<std::uint8_t>> &packets,
                  QString *error)
{
    const AVSampleFormat format = static_cast<AVSampleFormat>(context->sample_fmt);

    CBridge::FramePtr frame(CBridge::makeFrame());
    CBridge::PacketPtr packet(CBridge::makePacket());

    bool ok = true;
    const int totalSamples = kSampleRate * kDurationSeconds;
    for (int offset = 0; offset < totalSamples && ok; offset += kFrameSize) {
        // The encoder holds a reference to the frame's data while it is inside send/receive, so
        // release our copy after each iteration. av_frame_unref resets format, nb_samples and
        // ch_layout back to defaults, which must be restored (and the buffer re-allocated) before
        // the frame can be reused - sending an unreset frame makes avcodec_send_frame fail with
        // EINVAL deep inside av_frame_ref.
        av_frame_unref(frame.get());
        frame->format = format;
        frame->sample_rate = kSampleRate;
        av_channel_layout_copy(&frame->ch_layout, &context->ch_layout);
        frame->nb_samples = kFrameSize;
        if (av_frame_get_buffer(frame.get(), 0) < 0) {
            return false; // out of memory
        }

        fillToneFrame(frame.get(), std::size_t(offset));
        frame->pts = offset;
        const int sendRet = avcodec_send_frame(context.get(), frame.get());
        if (sendRet < 0) {
            if (error) {
                *error = u"avcodec_send_frame failed at sample %1: %2"_s.arg(offset).arg(
                    CBridge::avErrorString(sendRet));
            }
            ok = false;
            break;
        }

        while (true) {
            const int ret = avcodec_receive_packet(context.get(), packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                if (error) {
                    *error = u"avcodec_receive_packet failed at sample %1: %2"_s.arg(offset).arg(
                        CBridge::avErrorString(ret));
                }
                ok = false;
                break;
            }
            packets.emplace_back(packet->data, packet->data + packet->size);
            av_packet_unref(packet.get());
        }
    }

    // Flush the encoder's internal delay so no tail samples are lost.
    if (ok) {
        const int flushRet = avcodec_send_frame(context.get(), nullptr);
        if (flushRet >= 0) {
            while (true) {
                const int ret = avcodec_receive_packet(context.get(), packet.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                if (ret < 0) {
                    if (error) {
                        *error = u"avcodec_receive_packet failed while flushing: %1"_s.arg(
                            CBridge::avErrorString(ret));
                    }
                    ok = false;
                    break;
                }
                packets.emplace_back(packet->data, packet->data + packet->size);
                av_packet_unref(packet.get());
            }
        } else {
            if (error) {
                *error = u"avcodec_send_frame failed while flushing: %1"_s.arg(
                    CBridge::avErrorString(flushRet));
            }
            ok = false;
        }
    }

    return ok;
}

// Encodes kDurationSeconds of the reference tones into raw Opus packets, exactly what a
// WHEP source delivers per RTP payload.
bool encodeTone(int channels, std::vector<std::vector<std::uint8_t>> &packets, QString *error)
{
    // Prefer the libopus wrapper, fall back to the native opus encoder; either produces standard
    // RFC 6716 packets.
    const char *encoderNames[] = { "libopus", "opus" };

    for (const auto name : encoderNames) {
        const AVCodec *encoder = avcodec_find_encoder_by_name(name);
        if (!encoder || encoder->id != AV_CODEC_ID_OPUS) {
            continue;
        }

        // Collect the formats this encoder advertises, ordered by our preference; if it only
        // advertises an exotic format, fall back to the first one so the test still runs.
        std::vector<AVSampleFormat> candidates;
        auto addIfAdvertised = [&](AVSampleFormat fmt) {
            for (const AVSampleFormat *advertised = encoder->sample_fmts;
                 advertised && *advertised != AV_SAMPLE_FMT_NONE; ++advertised) {
                if (*advertised == fmt
                    && std::find(candidates.begin(), candidates.end(), fmt) == candidates.end()) {
                    candidates.push_back(fmt);
                    return;
                }
            }
        };
        addIfAdvertised(AV_SAMPLE_FMT_FLTP);
        addIfAdvertised(AV_SAMPLE_FMT_S16P);
        addIfAdvertised(AV_SAMPLE_FMT_S16);
        if (encoder->sample_fmts) {
            for (const AVSampleFormat *fmt = encoder->sample_fmts; *fmt != AV_SAMPLE_FMT_NONE;
                 ++fmt) {
                if (std::find(candidates.begin(), candidates.end(), *fmt) == candidates.end()) {
                    candidates.push_back(*fmt);
                }
            }
        }

        for (const auto sampleFormat : candidates) {
            CBridge::CodecContextPtr context(avcodec_alloc_context3(encoder));
            if (!context) {
                return false; // out of memory
            }
            context->sample_rate = kSampleRate;
            av_channel_layout_default(&context->ch_layout, channels);
            // Encoders take the input format in sample_fmt (request_sample_fmt is decoder-side).
            context->sample_fmt = sampleFormat;
            // The native opus encoder is experimental (the CLI's -strict -2).
            context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

            const int openRet = avcodec_open2(context.get(), encoder, nullptr);
            if (openRet < 0) {
                continue; // try the next advertised format
            }

            std::printf("  encoder: %s (%s)\n", encoder->name,
                        av_get_sample_fmt_name(static_cast<AVSampleFormat>(context->sample_fmt)));
            return driveEncoder(context, packets, error);
        }

        // Last resort for this encoder: let it pick its own format.
        CBridge::CodecContextPtr context(avcodec_alloc_context3(encoder));
        if (!context) {
            return false; // out of memory
        }
        context->sample_rate = kSampleRate;
        av_channel_layout_default(&context->ch_layout, channels);
        // The native opus encoder is experimental (the CLI's -strict -2).
        context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

        const int openRet = avcodec_open2(context.get(), encoder, nullptr);
        if (openRet >= 0) {
            std::printf("  encoder: %s (%s, default)\n", encoder->name,
                        av_get_sample_fmt_name(static_cast<AVSampleFormat>(context->sample_fmt)));
            return driveEncoder(context, packets, error);
        }
    }

    if (error) {
        *error = u"Could not open any Opus encoder in this FFmpeg build"_s;
    }
    return false;
}

// Loads raw Opus packets from an Ogg/Opus reference file (generated with a working FFmpeg:
// ffmpeg -f lavfi -i "sine=frequency=440:duration=2:r=48000" -af volume=4 -c:a opus -strict -2 ref.ogg).
// The ogg demuxer yields exact packet boundaries, which is what an RTP payload carries.
bool loadPacketsFromOgg(const QString &path, std::vector<std::vector<std::uint8_t>> &packets,
                        QString *error)
{
    AVFormatContext *context = nullptr;
    if (avformat_open_input(&context, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        if (error) {
            *error = u"Could not open the reference file %1"_s.arg(path);
        }
        return false;
    }
    if (avformat_find_stream_info(context, nullptr) < 0) {
        avformat_close_input(&context);
        if (error) {
            *error = u"Could not probe the reference file %1"_s.arg(path);
        }
        return false;
    }

    const int audioIndex = av_find_best_stream(context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIndex < 0) {
        avformat_close_input(&context);
        if (error) {
            *error = u"No audio stream in %1"_s.arg(path);
        }
        return false;
    }

    CBridge::PacketPtr packet(CBridge::makePacket());
    while (av_read_frame(context, packet.get()) >= 0) {
        if (packet->stream_index == audioIndex && packet->size > 0) {
            packets.emplace_back(packet->data, packet->data + packet->size);
        }
    }

    avformat_close_input(&context);
    return !packets.empty();
}

// Feeds the raw Opus packets through the production AudioDecoder exactly like NdiSink does.
bool decodeRoundTrip(const std::vector<std::vector<std::uint8_t>> &packets, int channels,
                     DecodedAudio &decoded, QString *error)
{
    CBridge::AudioDecoder decoder;
    if (!decoder.open(kSampleRate, channels, error)) {
        return false;
    }
    decoder.setFrameCallback([&decoded](AVFrame *frame) { collectFrames(frame, decoded); });

    std::uint32_t rtpTimestamp = 0; // the Opus RTP clock is 48 kHz
    for (const auto &packet : packets) {
        if (!decoder.decode(packet.data(), packet.size(), rtpTimestamp, error)) {
            return false;
        }
        rtpTimestamp += kFrameSize;
    }
    return true;
}

struct VerifyResult
{
    bool pass = false;
    double correlation = 0.0;
    double rmsDbfs = -300.0; // decoded level in dBFS over the scored window
    int lagSamples = -1;
};

// The decoded tone is delayed by the codec's internal delay, so search for the lag that best
// aligns it with the reference before scoring.
VerifyResult verifyTone(const DecodedAudio &decoded, int channel)
{
    VerifyResult result;
    const auto &dec = decoded.channels[channel];

    // Drop a fixed head: covers encoder/decoder delay plus any first-frame artifacts from
    // decoding a raw stream without container pre-skip metadata.
    constexpr std::size_t kHeadTrim = 4800; // 100 ms
    if (dec.size() <= kHeadTrim + std::size_t(kSampleRate) / 2) {
        return result; // not enough audio was decoded at all
    }

    const double frequency = toneFrequency(channel);
    std::vector<float> reference(std::size_t(kSampleRate) * kDurationSeconds);
    for (int i = 0; i < kSampleRate * kDurationSeconds; ++i) {
        reference[std::size_t(i)] = static_cast<float>(kAmplitude * std::sin(
            2.0 * std::numbers::pi * frequency * double(i) / kSampleRate));
    }

    const int window = kSampleRate / 2; // score over one second of audio
    constexpr int kMaxLag = 9600;       // codec delay is well below 200 ms

    double bestCorrelation = -2.0;
    for (int lag = 0; lag <= kMaxLag && std::size_t(lag) + std::size_t(window) < reference.size();
         ++lag) {
        double numerator = 0.0, decEnergy = 0.0, refEnergy = 0.0;
        for (int i = 0; i < window; ++i) {
            const float a = dec[kHeadTrim + std::size_t(i)];
            const float b = reference[std::size_t(lag) + std::size_t(i)];
            numerator += double(a) * double(b);
            decEnergy += double(a) * double(a);
            refEnergy += double(b) * double(b);
        }
        if (decEnergy > 0.0 && refEnergy > 0.0) {
            const double correlation = numerator / std::sqrt(decEnergy * refEnergy);
            if (correlation > bestCorrelation) {
                bestCorrelation = correlation;
                result.lagSamples = lag;
            }
        }
    }

    // Absolute level of the decoded audio: catches silence and near-zero gain without
    // depending on the input's own loudness, which a live stream does not guarantee.
    double decEnergy = 0.0;
    for (int i = 0; i < window; ++i) {
        const float a = dec[kHeadTrim + std::size_t(i)];
        decEnergy += double(a) * double(a);
    }
    const double rms = std::sqrt(decEnergy / window);
    result.rmsDbfs = rms > 0.0 ? 20.0 * std::log10(rms) : -300.0;

    result.correlation = bestCorrelation;
    // Opus is lossy, but a pure tone survives it almost intact: require strong correlation
    // (right shape, right frequency, right channel) and an audible level (> -40 dBFS).
    result.pass = bestCorrelation > 0.95 && rms > 0.01 && rms < 2.0;
    return result;
}

// Muxes the packets into an MPEG-TS file with exactly the parameters TsMulticastSink uses
// (Opus stream, 48 kHz time base, duration-less audio packets).
bool muxToTs(const std::vector<std::vector<std::uint8_t>> &packets, int channels, const QString &path,
             QString *error)
{
    AVFormatContext *muxer = nullptr;
    if (avformat_alloc_output_context2(&muxer, nullptr, "mpegts", path.toUtf8().constData()) < 0
        || !muxer) {
        if (error) {
            *error = u"Could not allocate the MPEG-TS muxer"_s;
        }
        return false;
    }

    AVStream *stream = avformat_new_stream(muxer, nullptr);
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_OPUS;
    stream->codecpar->sample_rate = kSampleRate;
    av_channel_layout_default(&stream->codecpar->ch_layout, channels);
    stream->time_base = AVRational { 1, kSampleRate };

    if (!(muxer->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open2(&muxer->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE, nullptr,
                       nullptr) < 0) {
            avformat_free_context(muxer);
            if (error) {
                *error = u"Could not open the TS output file"_s;
            }
            return false;
        }
    }

    const int headerRet = avformat_write_header(muxer, nullptr);
    if (headerRet < 0) {
        char buffer[AV_ERROR_MAX_STRING_SIZE] {};
        av_strerror(headerRet, buffer, sizeof(buffer));
        if (muxer->pb) {
            avio_closep(&muxer->pb);
        }
        avformat_free_context(muxer);
        if (error) {
            *error = u"MPEG-TS muxer rejected the Opus stream: %1"_s.arg(QString::fromUtf8(buffer));
        }
        return false;
    }

    CBridge::PacketPtr packet(CBridge::makePacket());
    bool ok = true;
    for (std::size_t i = 0; i < packets.size() && ok; ++i) {
        av_packet_unref(packet.get());
        if (av_new_packet(packet.get(), int(packets[i].size())) < 0) {
            ok = false;
            break;
        }
        std::memcpy(packet->data, packets[i].data(), packets[i].size());
        packet->stream_index = 0;
        packet->pts = std::int64_t(i) * kFrameSize;
        packet->dts = packet->pts;
        packet->duration = 0; // TsMulticastSink writes duration-less audio packets
        if (av_interleaved_write_frame(muxer, packet.get()) < 0) {
            ok = false;
        }
    }

    if (ok) {
        av_write_trailer(muxer);
    }
    if (muxer->pb) {
        avio_closep(&muxer->pb);
    }
    avformat_free_context(muxer);
    if (!ok) {
        if (error) {
            *error = u"Writing the TS file failed"_s;
        }
        return false;
    }
    return true;
}

// Muxes the packets into an MPEG-TS file with exactly the parameters TsMulticastSink uses,
// then demuxes and decodes it again to prove the container carries playable Opus.
bool tsRoundTrip(const std::vector<std::vector<std::uint8_t>> &packets, int channels,
                 DecodedAudio &decoded, QString *error)
{
    const QString path = QDir::tempPath() + u"/cbridge-audio-smoke.ts"_s;
    if (!muxToTs(packets, channels, path, error)) {
        return false;
    }

    // Demux and decode with the production decoder.
    AVFormatContext *demuxer = nullptr;
    if (avformat_open_input(&demuxer, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        if (error) {
            *error = u"Could not open the TS file for reading"_s;
        }
        return false;
    }
    if (avformat_find_stream_info(demuxer, nullptr) < 0) {
        avformat_close_input(&demuxer);
        if (error) {
            *error = u"Could not probe the TS file"_s;
        }
        return false;
    }

    const int audioIndex = av_find_best_stream(demuxer, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIndex < 0) {
        avformat_close_input(&demuxer);
        if (error) {
            *error = u"No audio stream found in the TS file"_s;
        }
        return false;
    }

    CBridge::AudioDecoder decoder;
    if (!decoder.open(kSampleRate, channels, error)) {
        avformat_close_input(&demuxer);
        return false;
    }
    decoder.setFrameCallback([&decoded](AVFrame *frame) { collectFrames(frame, decoded); });

    CBridge::PacketPtr packet(CBridge::makePacket());
    bool ok = true;
    while (av_read_frame(demuxer, packet.get()) >= 0) {
        if (packet->stream_index != audioIndex) {
            continue;
        }
        QString decodeError;
        if (!decoder.decode(packet->data, packet->size, packet->pts, &decodeError)) {
            ok = false;
            if (error) {
                *error = u"Decoding the demuxed TS audio failed: %1"_s.arg(decodeError);
            }
            break;
        }
    }

    avformat_close_input(&demuxer);
    std::remove(path.toUtf8().constData());
    return ok;
}

// --- Opus payload sanitizer checks ---------------------------------------------------------
//
// The live test stream's upstream encoder appends one undeclared trailing byte to most of its CBR
// two-frame (code mode 1) Opus payloads, which libopus rejects outright. These checks prove that
// OpusPayloadSanitizer recovers such packets structurally and that multi-frame Opus payloads
// survive the TS round trip exactly as TsMulticastSink delivers them; analyzeRawCapture() does the
// same end-to-end proof against a real capture of the defective stream.

int runSanitizerChecks()
{
    std::printf("== Opus payload sanitizer ==\n");
    int failures = 0;
    auto check = [&](const char *name, bool ok) {
        std::printf("  %s: %s\n", name, ok ? "PASS" : "FAIL");
        if (!ok) {
            ++failures;
        }
    };

    // Mode 0 (one frame): any payload up to 1275 bytes is structurally valid; a packet one byte
    // over the limit is recovered by stripping the stray byte, like the CBR defect.
    {
        std::vector<std::uint8_t> p(30, 0xAB);
        p[0] = 0x7C; // config 15, stereo, one frame
        check("mode 0: well-formed packet stays untouched",
              CBridge::OpusPayloadSanitizer::validPrefixLength(p.data(), p.size()) == p.size());

        std::vector<std::uint8_t> over(1 + 1276, 0xAB); // one byte over the frame limit
        over[0] = 0x7C;
        check("mode 0: one byte over the limit is recovered by stripping it",
              !CBridge::OpusPayloadSanitizer::structurallyValid(over.data(), over.size())
                  && CBridge::OpusPayloadSanitizer::validPrefixLength(over.data(), over.size()) == over.size() - 1);

        std::vector<std::uint8_t> big(1 + 1280, 0xAB); // beyond what a 4-byte strip can fix
        big[0] = 0x7C;
        check("mode 0: far over the limit is not recoverable by trimming",
              !CBridge::OpusPayloadSanitizer::structurallyValid(big.data(), big.size())
                  && CBridge::OpusPayloadSanitizer::validPrefixLength(big.data(), big.size()) == big.size());
    }

    // Mode 1 (two CBR frames): the observed defect. An odd payload after the TOC byte is invalid;
    // stripping one trailing byte recovers it.
    {
        std::vector<std::uint8_t> p(1 + 2 * 40, 0xAB);
        p[0] = 0x7D; // config 15, stereo, two CBR frames
        check("mode 1: even payload stays untouched",
              CBridge::OpusPayloadSanitizer::validPrefixLength(p.data(), p.size()) == p.size());

        p.push_back(0xFF); // the stray trailing byte
        check("mode 1: odd payload is recovered by stripping one byte",
              !CBridge::OpusPayloadSanitizer::structurallyValid(p.data(), p.size())
                  && CBridge::OpusPayloadSanitizer::validPrefixLength(p.data(), p.size()) == p.size() - 1);
    }

    // Mode 2 (two VBR frames): explicit size prefix.
    {
        std::vector<std::uint8_t> p = {0x7A, 5, 1, 2, 3, 4, 5, 6, 7, 8}; // first frame 5 bytes, second 3
        check("mode 2: well-formed VBR pair stays untouched",
              CBridge::OpusPayloadSanitizer::validPrefixLength(p.data(), p.size()) == p.size());

        std::vector<std::uint8_t> truncated = {0x7A, 5, 1, 2}; // declared frame does not fit
        check("mode 2: truncated VBR pair is not recoverable by trimming",
              !CBridge::OpusPayloadSanitizer::structurallyValid(truncated.data(), truncated.size())
                  && CBridge::OpusPayloadSanitizer::validPrefixLength(truncated.data(), truncated.size()) == truncated.size());
    }

    // Mode 3 (multiple CBR frames): the remainder must divide evenly by the frame count.
    {
        std::vector<std::uint8_t> p(1 + 1 + 4 * 10, 0xAB);
        p[0] = 0x67; // config 12, stereo, four frames
        p[1] = 4;    // count=4, CBR (bit 7 clear), no padding
        check("mode 3: evenly divisible CBR group stays untouched",
              CBridge::OpusPayloadSanitizer::validPrefixLength(p.data(), p.size()) == p.size());

        p.push_back(0xFF); // remainder is now 41, not divisible by 4
        check("mode 3: indivisible CBR group is recovered by stripping one byte",
              CBridge::OpusPayloadSanitizer::validPrefixLength(p.data(), p.size()) == p.size() - 1);
    }

    // Mode 3 with the padding flag set: declared padding bytes are consumed before frame sizes.
    {
        std::vector<std::uint8_t> p;
        p.push_back(0x67); // config 12, stereo, CC=3
        p.push_back(0x42); // count=2 (bits 0-5), padding flag set (bit 6)
        p.push_back(3);    // declares 3 bytes of padding
        for (int i = 0; i < 3; ++i) {
            p.push_back(0xCD); // the declared padding bytes themselves
        }
        for (int i = 0; i < 8; ++i) {
            p.push_back(0xAB); // 8 payload bytes -> two CBR frames of 4 bytes each
        }
        check("mode 3: declared padding is consumed, packet stays untouched",
              CBridge::OpusPayloadSanitizer::validPrefixLength(p.data(), p.size()) == p.size());

        std::vector<std::uint8_t> shortPad = {0x67, 0x42, 5}; // declares more padding than exists
        check("mode 3: over-declared padding is not recoverable by trimming",
              !CBridge::OpusPayloadSanitizer::structurallyValid(shortPad.data(), shortPad.size())
                  && CBridge::OpusPayloadSanitizer::validPrefixLength(shortPad.data(), shortPad.size()) == shortPad.size());
    }

    // Mode 3 VBR (bit 7 set): count-1 explicit size prefixes first, then the frame payloads; the
    // last frame absorbs whatever is left. A trailing stray byte just grows that last frame, so it
    // stays structurally valid - the one case the sanitizer cannot detect by structure alone.
    {
        std::vector<std::uint8_t> p;
        p.push_back(0x67); // config 12, stereo, CC=3
        p.push_back(0x84); // count=4 (bits 0-5), VBR flag set (bit 7)
        p.push_back(5);    // frame 1: 5 bytes
        p.push_back(2);    // frame 2: 2 bytes
        p.push_back(3);    // frame 3: 3 bytes
        for (int i = 0; i < 5; ++i) {
            p.push_back(0xAB); // frame 1 data
        }
        for (int i = 0; i < 2; ++i) {
            p.push_back(0xAB); // frame 2 data
        }
        for (int i = 0; i < 3; ++i) {
            p.push_back(0xAB); // frame 3 data
        }
        for (int i = 0; i < 4; ++i) {
            p.push_back(0xAB); // frame 4 absorbs the rest: 4 bytes
        }
        check("mode 3 VBR: well-formed group stays untouched",
              CBridge::OpusPayloadSanitizer::validPrefixLength(p.data(), p.size()) == p.size());

        p.push_back(0xFF); // trailing byte is absorbed by the last frame - undetectable
        check("mode 3 VBR: trailing byte stays inside the last frame (undetectable)",
              CBridge::OpusPayloadSanitizer::validPrefixLength(p.data(), p.size()) == p.size());

        std::vector<std::uint8_t> truncated = {0x67, 0x84, 5}; // declared frame does not fit
        check("mode 3 VBR: truncated group is not recoverable by trimming",
              !CBridge::OpusPayloadSanitizer::structurallyValid(truncated.data(), truncated.size())
                  && CBridge::OpusPayloadSanitizer::validPrefixLength(truncated.data(), truncated.size()) == truncated.size());
    }

    // Empty payload.
    {
        std::vector<std::uint8_t> empty;
        check("empty packet maps to zero",
              CBridge::OpusPayloadSanitizer::validPrefixLength(empty.data(), empty.size()) == 0);
    }

    // End-to-end: pack consecutive encoded tone frames into two-frame code-mode-2 (VBR pair)
    // packets - the same multi-frame shape as the live stream's two-frames-per-packet payloads,
    // but without any padding so every frame stays intact. This proves that multi-frame Opus
    // payloads survive the TS round trip exactly as TsMulticastSink delivers them. The live
    // stream's specific defect (mode-1 CBR pairs with one undeclared trailing byte) cannot be
    // synthesised faithfully from VBR frames: equal-size halves would require zero-padding the
    // shorter frame, and padding corrupts hybrid/SILK frames through their length-dependent
    // in-band redundancy signalling. That case is covered by the structural checks above plus
    // analyzeRawCapture() on a real dump, which decodes the sanitized stream end to end.
    {
        std::vector<std::vector<std::uint8_t>> source;
        QString error;
        if (!encodeTone(2, source, &error)) {
            std::printf("  (skipping multi-frame end-to-end: %s)\n", qUtf8Printable(error));
        } else {
            // Only single-frame packets can be repacked. Skip the encoder's warm-up packet and
            // require enough pairs for verifyTone to have audio to score.
            bool pairable = source.size() >= 10;
            for (std::size_t i = 2; pairable && i + 1 < source.size(); i += 2) {
                if ((source[i][0] & 3) != 0 || source[i].size() < 2
                    || (source[i][0] & 0xFC) != (source[i + 1][0] & 0xFC)) {
                    pairable = false;
                }
            }
            if (!pairable) {
                std::printf("  (skipping multi-frame end-to-end: encoder did not emit single-frame packets)\n");
            } else {
                auto sizePrefix = [](std::size_t len, std::vector<std::uint8_t> &out) {
                    if (len < 252) {
                        out.push_back(static_cast<std::uint8_t>(len));
                    } else {
                        // libopus two-byte form: low bits first (see encode_size in opus.c).
                        out.push_back(static_cast<std::uint8_t>(252 + (len & 3)));
                        out.push_back(static_cast<std::uint8_t>((len - 252) >> 2));
                    }
                };

                std::vector<std::vector<std::uint8_t>> pairs;
                for (std::size_t i = 2; i + 1 < source.size(); i += 2) {
                    const auto &a = source[i];
                    const auto &b = source[i + 1];
                    std::vector<std::uint8_t> packet;
                    packet.push_back(static_cast<std::uint8_t>((a[0] & 0xFC) | 2)); // two VBR frames
                    sizePrefix(a.size() - 1, packet);
                    packet.insert(packet.end(), a.begin() + 1, a.end());
                    packet.insert(packet.end(), b.begin() + 1, b.end());
                    pairs.push_back(std::move(packet));
                }

                bool allValid = true;
                for (const auto &p : pairs) {
                    if (!CBridge::OpusPayloadSanitizer::structurallyValid(p.data(), p.size())) {
                        allValid = false;
                    }
                }
                check("multi-frame packets are structurally valid as-is", allValid);

                DecodedAudio decoded;
                if (!tsRoundTrip(pairs, 2, decoded, &error)) {
                    std::printf("  FAIL: multi-frame TS round trip: %s\n", qUtf8Printable(error));
                    ++failures;
                } else {
                    for (int c = 0; c < 2; ++c) {
                        const VerifyResult result = verifyTone(decoded, c);
                        std::printf(
                            "  multi-frame TS round trip (ch %d): correlation=%.4f level=%.1f dBFS -> %s\n",
                            c, result.correlation, result.rmsDbfs, result.pass ? "PASS" : "FAIL");
                        if (!result.pass) {
                            ++failures;
                        }
                    }
                }
            }
        }
    }

    return failures;
}

// Offline analysis of a webrtc_audio_packets.raw dump ([uint32 LE rtpTs][uint32 LE size][payload]):
// classifies every packet as structurally valid as-is, recoverable by stripping trailing bytes, or
// still invalid; then sanitizes the whole stream and decodes it with the production decoder to prove
// that a receiver without retry logic can play it (zero decode failures, zero trailing-byte retries,
// audible level). Finally muxes the sanitized packets into <name>_sanitized.ts next to the dump so
// the result can be checked in VLC. Used to validate the sanitizer against real captures.
int analyzeRawCapture(const char *path)
{
    std::printf("C-Bridge Opus payload analysis: %s\n", path);

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("could not open the capture file\n");
        return 1;
    }

    struct Record
    {
        std::uint32_t rtpTs = 0;
        std::vector<std::uint8_t> payload;
    };
    std::vector<Record> records;

    long total = 0, validAsIs = 0, recovered = 0, unrecoverable = 0;
    long strippedBytes = 0;
    int firstUnrecoverableToc = -1;
    while (true) {
        std::uint32_t ts = 0, size = 0;
        file.read(reinterpret_cast<char *>(&ts), sizeof(ts));
        if (!file) {
            break; // clean end of file
        }
        file.read(reinterpret_cast<char *>(&size), sizeof(size));
        if (!file || size == 0 || size > 65536) {
            std::printf("truncated or corrupt record at packet %ld\n", total);
            return 1;
        }
        Record record;
        record.rtpTs = ts;
        record.payload.resize(size);
        file.read(reinterpret_cast<char *>(record.payload.data()), size);
        if (!file) {
            std::printf("truncated payload at packet %ld\n", total);
            return 1;
        }

        ++total;
        const bool valid = CBridge::OpusPayloadSanitizer::structurallyValid(record.payload.data(),
                                                                            record.payload.size());
        const std::size_t prefix = CBridge::OpusPayloadSanitizer::validPrefixLength(
            record.payload.data(), record.payload.size());
        if (valid) {
            ++validAsIs;
        } else if (prefix < record.payload.size()) {
            ++recovered;
            strippedBytes += long(record.payload.size() - prefix);
        } else {
            ++unrecoverable;
            if (firstUnrecoverableToc < 0 && !record.payload.empty()) {
                firstUnrecoverableToc = int(record.payload[0]);
            }
        }
        records.push_back(std::move(record));
    }

    if (total == 0) {
        std::printf("no packets found\n");
        return 1;
    }

    std::printf("packets: %ld\n", total);
    std::printf("structurally valid as-is:   %ld (%.1f%%)\n", validAsIs,
                100.0 * double(validAsIs) / double(total));
    std::printf("recovered by stripping:     %ld (%.1f%%, %ld bytes stripped)\n", recovered,
                100.0 * double(recovered) / double(total), strippedBytes);
    std::printf("still invalid after strip:  %ld%s\n", unrecoverable,
                firstUnrecoverableToc >= 0 ? " (first TOC byte: 0x%02X)" : "");

    // Decode the sanitized stream with the production decoder. A receiver without retry logic can
    // play it only if every packet decodes on its first attempt, so any trailing-byte retry here is
    // a sanitizer miss. The channel count comes from the TOC stereo bit of the first packet.
    int failures = 0;
    const int channels = records.front().payload.empty() ? 2 : (records.front().payload[0] & 0x80) ? 2 : 1;

    CBridge::AudioDecoder decoder;
    QString error;
    DecodedAudio decoded;
    long decodeFailures = 0;
    if (!decoder.open(kSampleRate, channels, &error)) {
        std::printf("decode: could not open the production decoder: %s\n", qUtf8Printable(error));
        return 1;
    }
    decoder.setFrameCallback([&decoded](AVFrame *frame) { collectFrames(frame, decoded); });
    for (const auto &record : records) {
        const std::size_t prefix = CBridge::OpusPayloadSanitizer::validPrefixLength(
            record.payload.data(), record.payload.size());
        if (!decoder.decode(record.payload.data(), prefix, std::int64_t(record.rtpTs), &error)) {
            ++decodeFailures;
        }
    }

    const long retries = long(decoder.trailingByteRetryCount());
    double rms = 0.0;
    if (!decoded.channels[0].empty()) {
        double energy = 0.0;
        for (const float sample : decoded.channels[0]) {
            energy += double(sample) * double(sample);
        }
        rms = std::sqrt(energy / double(decoded.channels[0].size()));
    }
    const double levelDbfs = rms > 0.0 ? 20.0 * std::log10(rms) : -300.0;

    std::printf("sanitized decode: %ld packets, %ld failures, %ld trailing-byte retries, "
                "%zu samples, level=%.1f dBFS\n",
                total, decodeFailures, retries, decoded.channels[0].size(), levelDbfs);
    if (decodeFailures > 0) {
        ++failures;
    }
    if (retries > 0) {
        ++failures;
    }
    if (levelDbfs < -60.0) {
        ++failures;
    }

    // Mux the sanitized stream with TsMulticastSink's parameters so it can be played in VLC.
    std::vector<std::vector<std::uint8_t>> sanitized;
    sanitized.reserve(records.size());
    for (const auto &record : records) {
        const std::size_t prefix = CBridge::OpusPayloadSanitizer::validPrefixLength(
            record.payload.data(), record.payload.size());
        sanitized.emplace_back(record.payload.begin(), record.payload.begin() + prefix);
    }
    const QFileInfo info(QString::fromUtf8(path));
    const QString outPath = info.absolutePath() + u"/" + info.completeBaseName() + u"_sanitized.ts";
    if (!muxToTs(sanitized, channels, outPath, &error)) {
        std::printf("warning: could not write the sanitized TS file: %s\n", qUtf8Printable(error));
    } else {
        std::printf("sanitized stream written to: %s (play it in VLC)\n",
                    outPath.toUtf8().constData());
    }

    if (failures == 0) {
        std::printf("ANALYSIS PASSED\n");
        return 0;
    }
    std::printf("ANALYSIS FAILED (%d problem group(s))\n", failures);
    return 1;
}

int runCase(const QString &name, int channels, const char *referenceFile)
{
    std::printf("== %s ==\n", qUtf8Printable(name));
    QString error;

    std::vector<std::vector<std::uint8_t>> packets;
    if (referenceFile) {
        if (!loadPacketsFromOgg(QString::fromUtf8(referenceFile), packets, &error)) {
            std::printf("  FAIL: load reference: %s\n", qUtf8Printable(error));
            return 1;
        }
        std::printf("  loaded %zu Opus packets from %s\n", packets.size(), referenceFile);
    } else if (!encodeTone(channels, packets, &error)) {
        std::printf("  FAIL: encode: %s (pass an .ogg reference file to use external packets)\n",
                    qUtf8Printable(error));
        return 1;
    } else {
        std::printf("  encoded %zu Opus packets (%d s @ %d Hz, %d ch)\n", packets.size(),
                    kDurationSeconds, kSampleRate, channels);
    }

    DecodedAudio decoded;
    if (!decodeRoundTrip(packets, channels, decoded, &error)) {
        std::printf("  FAIL: decode: %s\n", qUtf8Printable(error));
        return 1;
    }
    std::printf("  decoded %ld frames -> %zu samples, format=%s rate=%d ch=%d\n",
                decoded.frameCount, decoded.channels[0].size(),
                decoded.formatOk ? "FLTP" : "unexpected", decoded.sampleRate,
                decoded.channelCount);

    if (!decoded.formatOk || decoded.sampleRate != kSampleRate || decoded.channelCount != channels) {
        std::printf("  FAIL: decoded frames do not match the expected FLTP/%d Hz/%d ch contract\n",
                    kSampleRate, channels);
        return 1;
    }

    int failures = 0;
    for (int c = 0; c < channels; ++c) {
        const VerifyResult result = verifyTone(decoded, c);
        std::printf("  channel %d (%.0f Hz): correlation=%.4f level=%.1f dBFS lag=%d ms -> %s\n",
                    c, toneFrequency(c), result.correlation, result.rmsDbfs,
                    result.lagSamples >= 0 ? int(result.lagSamples / (kSampleRate / 1000.0)) : -1,
                    result.pass ? "PASS" : "FAIL");
        if (!result.pass) {
            ++failures;
        }
    }

    // TS passthrough: the same packets through the container TsMulticastSink produces.
    DecodedAudio tsDecoded;
    if (!tsRoundTrip(packets, channels, tsDecoded, &error)) {
        std::printf("  FAIL: TS round trip: %s\n", qUtf8Printable(error));
        return 1;
    }
    const VerifyResult tsResult = verifyTone(tsDecoded, 0);
    std::printf("  TS file round trip (ch 0): correlation=%.4f level=%.1f dBFS -> %s\n",
                tsResult.correlation, tsResult.rmsDbfs, tsResult.pass ? "PASS" : "FAIL");
    if (!tsResult.pass) {
        ++failures;
    }

    return failures == 0 ? 0 : 1;
}

} // namespace

// Prints the sample formats each Opus encoder advertises through FFmpeg 8's config API,
// under normal and experimental compliance. Diagnostic only.
void dumpSupportedConfigs()
{
    const char *names[] = { "libopus", "opus" };
    for (const auto name : names) {
        const AVCodec *codec = avcodec_find_encoder_by_name(name);
        if (!codec || codec->id != AV_CODEC_ID_OPUS) {
            std::printf("  %s: not found\n", name);
            continue;
        }
        for (int compliance : {FF_COMPLIANCE_NORMAL, FF_COMPLIANCE_EXPERIMENTAL}) {
            CBridge::CodecContextPtr probe(avcodec_alloc_context3(codec));
            if (!probe) {
                continue;
            }
            probe->strict_std_compliance = compliance;

            const void *configs = nullptr;
            int numConfigs = 0;
            const int ret = avcodec_get_supported_config(probe.get(), codec,
                                                         AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                                         &configs, &numConfigs);
            std::printf("  %s (compliance=%d): ret=%d", name, compliance, ret);
            if (ret == 0 && configs) {
                const int *formats = static_cast<const int *>(configs);
                for (int i = 0; i < numConfigs; ++i) {
                    std::printf(" %s", av_get_sample_fmt_name(static_cast<AVSampleFormat>(formats[i])));
                }
            } else if (ret == 0 && !configs) {
                std::printf(" (all formats supported)");
            }
            std::printf("\n");
        }
    }
}

int main(int argc, char *argv[])
{
    std::printf("C-Bridge audio smoke test\n");

    const AVCodec *libopusDecoder = avcodec_find_decoder_by_name("libopus");
    if (libopusDecoder && libopusDecoder->id == AV_CODEC_ID_OPUS) {
        std::printf("decoder: libopus wrapper\n");
    } else {
        std::printf("decoder: internal opus decoder (synthesised OpusHead)\n");
    }

    // First verify the production decoder opens at all - if this fails, no audio can ever flow.
    {
        CBridge::AudioDecoder probe;
        QString error;
        if (!probe.open(48000, 2, &error)) {
            std::printf("FATAL: AudioDecoder::open failed: %s\n", qUtf8Printable(error));
            return 1;
        }
        std::printf("AudioDecoder::open(48000, stereo): OK (rate=%d ch=%d)\n", probe.sampleRate(),
                    probe.channels());
    }

    dumpSupportedConfigs();

    // Optional arguments: an .raw packet capture for offline sanitizer analysis (which also decodes
    // the sanitized stream and writes a playable <name>_sanitized.ts), or Ogg/Opus reference files
    // (stereo first, then mono). Without them the test encodes its own packets in-process.
    if (argc > 1 && QString::fromUtf8(argv[1]).endsWith(QStringLiteral(".raw"))) {
        return analyzeRawCapture(argv[1]);
    }
    const char *stereoReference = argc > 1 ? argv[1] : nullptr;
    const char *monoReference = argc > 2 ? argv[2] : nullptr;

    int failures = 0;
    failures += runSanitizerChecks();
    failures += runCase(u"NDI path: stereo Opus decode"_s, 2, stereoReference);
    failures += runCase(u"NDI path: mono Opus decode"_s, 1, monoReference);

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
    } else {
        std::printf("%d CHECK(S) FAILED\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
