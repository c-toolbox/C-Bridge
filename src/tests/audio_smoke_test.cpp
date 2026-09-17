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
//
// Build and run:
//   cmake --build build --target audio-smoke-test --config Release
//   audio-smoke-test.exe [stereo_ref.ogg] [mono_ref.ogg]

#include "media/audiodecoder.h"
#include "media/avwrappers.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <QDir>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
bool driveEncoder(CBridge::CodecContextPtr &context, std::vector<std::vector<std::uint8_t>> &packets)
{
    const AVSampleFormat format = static_cast<AVSampleFormat>(context->sample_fmt);

    CBridge::FramePtr frame(CBridge::makeFrame());
    CBridge::PacketPtr packet(CBridge::makePacket());
    frame->format = format;
    frame->sample_rate = kSampleRate;
    av_channel_layout_copy(&frame->ch_layout, &context->ch_layout);
    frame->nb_samples = kFrameSize;
    if (av_frame_get_buffer(frame.get(), 0) < 0) {
        return false; // out of memory
    }

    bool ok = true;
    const int totalSamples = kSampleRate * kDurationSeconds;
    for (int offset = 0; offset < totalSamples && ok; offset += kFrameSize) {
        fillToneFrame(frame.get(), std::size_t(offset));
        frame->pts = offset;
        if (avcodec_send_frame(context.get(), frame.get()) < 0) {
            ok = false;
            break;
        }
        av_frame_unref(frame.get());

        while (true) {
            const int ret = avcodec_receive_packet(context.get(), packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                ok = false;
                break;
            }
            packets.emplace_back(packet->data, packet->data + packet->size);
            av_packet_unref(packet.get());
        }
    }

    // Flush the encoder's internal delay so no tail samples are lost.
    if (ok && avcodec_send_frame(context.get(), nullptr) >= 0) {
        while (true) {
            const int ret = avcodec_receive_packet(context.get(), packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                ok = false;
                break;
            }
            packets.emplace_back(packet->data, packet->data + packet->size);
            av_packet_unref(packet.get());
        }
    }

    return ok;
}

// Encodes kDurationSeconds of the reference tones into raw Opus packets, exactly what a
// WHEP source delivers per RTP payload.
bool encodeTone(int channels, std::vector<std::vector<std::uint8_t>> &packets, QString *error)
{
    // Prefer the libopus wrapper, but some builds ship a broken one (this build's refuses to
    // open in any format); either encoder produces standard RFC 6716 packets.
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
            context->request_sample_fmt = sampleFormat;
            // The native opus encoder is experimental (the CLI's -strict -2).
            context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

            const int openRet = avcodec_open2(context.get(), encoder, nullptr);
            if (openRet < 0) {
                continue; // try the next advertised format
            }

            std::printf("  encoder: %s (%s)\n", encoder->name,
                        av_get_sample_fmt_name(static_cast<AVSampleFormat>(context->sample_fmt)));
            return driveEncoder(context, packets);
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
            return driveEncoder(context, packets);
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

// Muxes the packets into an MPEG-TS file with exactly the parameters TsMulticastSink uses,
// then demuxes and decodes it again to prove the container carries playable Opus.
bool tsRoundTrip(const std::vector<std::vector<std::uint8_t>> &packets, int channels,
                 DecodedAudio &decoded, QString *error)
{
    const QString path = QDir::tempPath() + u"/cbridge-audio-smoke.ts"_s;

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

    ok = true;
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

    // Optional arguments: Ogg/Opus reference files (stereo first, then mono). Without them the
    // test encodes its own packets in-process.
    const char *stereoReference = argc > 1 ? argv[1] : nullptr;
    const char *monoReference = argc > 2 ? argv[2] : nullptr;

    int failures = 0;
    failures += runCase(u"NDI path: stereo Opus decode"_s, 2, stereoReference);
    failures += runCase(u"NDI path: mono Opus decode"_s, 1, monoReference);

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
    } else {
        std::printf("%d CHECK(S) FAILED\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
