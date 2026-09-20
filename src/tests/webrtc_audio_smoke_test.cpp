/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Live WebRTC audio smoke test.
//
// Connects to a running WHEP endpoint with the production WebRtcSource - exactly the code
// path StreamPipeline uses (WHEP signaling, libdatachannel RTP, OpusAudioDepacketizer) - and
// captures both the raw RTP datagrams (header included, for padding diagnostics) and the
// depacketized Opus payloads for N seconds, then:
//   1. decodes them with the production AudioDecoder and verifies format/level;
//   2. dumps the decoded PCM to a WAV file so you can listen to it yourself;
//   3. muxes the captured packets into an MPEG-TS file with exactly the parameters
//      TsMulticastSink uses, so you can check in VLC what multicast delivers. The TS mux applies
//      OpusPayloadSanitizer first, mirroring StreamPipeline::handleAudioUnit, which strips the
//      undeclared trailing bytes some upstream encoders append to their CBR Opus payloads.
//
// Build and run:
//   cmake --build build --target webrtc-audio-smoke-test --config Release
//   webrtc-audio-smoke-test.exe [whepUrl] [captureSeconds=10] [outputDir=.] [user] [password]

#include "media/audiodecoder.h"
#include "media/avwrappers.h"
#include "media/opuspayloadsanitizer.h"
#include "webrtc/webrtcsource.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
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

struct CapturedPacket
{
    std::vector<std::uint8_t> data;
    std::int64_t rtpTimestamp = 0; // Opus RTP clock is 48 kHz
};

// Wire-level view of one raw audio RTP datagram (header included), used to verify that RFC 3550
// padding is stripped before the payload reaches the decoder.
struct RawRtpPacket
{
    std::vector<std::uint8_t> data; // full datagram, header first
    bool padded = false;            // P bit set
    int padCount = 0;               // value of the trailing padding byte (includes itself)
    int payloadLength = 0;          // bytes after the RTP header, before any stripping
};

// Aggregated wire statistics over all captured raw datagrams.
struct RtpWireStats
{
    std::uint64_t totalPackets = 0;
    std::uint64_t paddedPackets = 0;
    std::uint64_t strippedBytes = 0;
    int minPadCount = -1;
    int maxPadCount = 0;
    std::map<int, std::uint64_t> padHistogram; // pad count -> packet count (padded only)
};

// Parses the fixed RTP header plus CSRC list and extension headers of a raw datagram.
void parseRtpHeader(RawRtpPacket &packet, RtpWireStats *stats)
{
    const auto &data = packet.data;
    if (data.size() < 12) {
        return;
    }
    const int csrcCount = data[0] & 0x0F;
    std::size_t headerSize = 12 + 4 * std::size_t(csrcCount);
    if ((data[0] & 0x10) != 0 && data.size() >= headerSize + 2) { // X bit: extension header present
        const std::uint8_t profile = data[headerSize];
        if (profile < 0x80) { // one-byte extension header: length in 32-bit words, excluding itself
            headerSize += 2 + 4 * std::size_t(data[headerSize + 1]);
        } else if (data.size() >= headerSize + 4) { // two-byte extension header
            const std::uint32_t words = (std::uint32_t(data[headerSize + 2]) << 8) | data[headerSize + 3];
            headerSize += 4 + 4 * std::size_t(words);
        }
    }
    if (data.size() < headerSize) {
        return; // malformed; nothing meaningful to measure
    }

    packet.payloadLength = int(data.size()) - int(headerSize);
    if ((data[0] & 0x20) != 0) { // P bit: last byte holds the padding length, including itself
        packet.padded = true;
        packet.padCount = data.back();
    }

    if (stats) {
        ++stats->totalPackets;
        if (packet.padded) {
            ++stats->paddedPackets;
            stats->strippedBytes += std::uint64_t(packet.padCount);
            stats->minPadCount = stats->minPadCount < 0 ? packet.padCount : std::min(stats->minPadCount, packet.padCount);
            stats->maxPadCount = std::max(stats->maxPadCount, packet.padCount);
            ++stats->padHistogram[packet.padCount];
        }
    }
}

const char *stateName(CBridge::StreamState state)
{
    switch (state) {
    case CBridge::StreamState::Idle: return "Idle";
    case CBridge::StreamState::Connecting: return "Connecting";
    case CBridge::StreamState::Running: return "Running";
    case CBridge::StreamState::Retrying: return "Retrying";
    case CBridge::StreamState::Failed: return "Failed";
    case CBridge::StreamState::Stopping: return "Stopping";
    }
    return "?";
}

const char *videoCodecName(CBridge::VideoCodec codec)
{
    switch (codec) {
    case CBridge::VideoCodec::H264: return "h264";
    case CBridge::VideoCodec::H265: return "hevc";
    default: return "unknown";
    }
}

// Writes the planar float capture as a 16-bit PCM WAV so it can be played back directly.
bool writePcm16Wav(const QString &path, const std::vector<std::vector<float>> &channels,
                   int sampleRate)
{
    if (channels.empty() || channels[0].empty()) {
        return false;
    }
    const int channelCount = int(channels.size());
    const auto frameCount = channels[0].size();

    std::ofstream file(path.toUtf8().constData(), std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }

    const std::uint32_t dataSize = std::uint32_t(frameCount) * std::uint32_t(channelCount) * 2u;
    auto writeLe = [&file](std::uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            file.put(char((value >> (8 * i)) & 0xFF));
        }
    };

    file.write("RIFF", 4);
    writeLe(36u + dataSize, 4);
    file.write("WAVEfmt ", 8);
    writeLe(16u, 4); // fmt chunk size (PCM)
    writeLe(1u, 2);   // audio format: PCM
    writeLe(std::uint64_t(channelCount), 2);
    writeLe(std::uint64_t(sampleRate), 4);
    writeLe(std::uint64_t(sampleRate) * std::uint64_t(channelCount) * 2u, 4); // byte rate
    writeLe(std::uint64_t(channelCount) * 2u, 2);                             // block align
    writeLe(16u, 2);                                                          // bits per sample
    file.write("data", 4);
    writeLe(dataSize, 4);

    for (auto i = 0; i < frameCount; ++i) {
        for (int c = 0; c < channelCount; ++c) {
            const float value = channels[c][std::size_t(i)];
            const int clamped = std::max(-1.0f, std::min(1.0f, value));
            const std::int16_t sample = std::int16_t(clamped * 32767.0f);
            file.write(reinterpret_cast<const char *>(&sample), sizeof(sample));
        }
    }
    return bool(file);
}

// Muxes the captured packets into an MPEG-TS file with exactly the parameters
// TsMulticastSink uses (Opus stream, 48 kHz time base, RTP timestamps as PTS/DTS). Like
// StreamPipeline::handleAudioUnit, each payload is first run through OpusPayloadSanitizer so the
// artifact matches what multicast actually delivers; trim statistics come back in *trimmedPackets
// and *trimmedBytes when non-null.
bool writeTsFile(const QString &path, const std::vector<CapturedPacket> &packets, int sampleRate,
                 int channelCount, std::uint64_t *trimmedPackets = nullptr,
                 std::uint64_t *trimmedBytes = nullptr)
{
    if (trimmedPackets) {
        *trimmedPackets = 0;
    }
    if (trimmedBytes) {
        *trimmedBytes = 0;
    }

    AVFormatContext *muxer = nullptr;
    if (avformat_alloc_output_context2(&muxer, nullptr, "mpegts", path.toUtf8().constData()) < 0
        || !muxer) {
        return false;
    }

    AVStream *stream = avformat_new_stream(muxer, nullptr);
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_OPUS;
    stream->codecpar->sample_rate = sampleRate;
    av_channel_layout_default(&stream->codecpar->ch_layout, channelCount);
    stream->time_base = AVRational { 1, sampleRate };

    if (!(muxer->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open2(&muxer->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE, nullptr,
                       nullptr) < 0) {
            avformat_free_context(muxer);
            return false;
        }
    }

    const int headerRet = avformat_write_header(muxer, nullptr);
    if (headerRet < 0) {
        if (muxer->pb) {
            avio_closep(&muxer->pb);
        }
        avformat_free_context(muxer);
        return false;
    }

    CBridge::PacketPtr packet(CBridge::makePacket());
    bool ok = true;
    for (const auto &captured : packets) {
        const std::size_t payloadSize =
            CBridge::OpusPayloadSanitizer::validPrefixLength(captured.data.data(), captured.data.size());
        if (payloadSize != captured.data.size()) {
            if (trimmedPackets) {
                ++*trimmedPackets;
            }
            if (trimmedBytes) {
                *trimmedBytes += captured.data.size() - payloadSize;
            }
        }

        av_packet_unref(packet.get());
        if (av_new_packet(packet.get(), int(payloadSize)) < 0) {
            ok = false;
            break;
        }
        std::memcpy(packet->data, captured.data.data(), payloadSize);
        packet->stream_index = 0;
        packet->pts = captured.rtpTimestamp;
        packet->dts = captured.rtpTimestamp;
        if (av_interleaved_write_frame(muxer, packet.get()) < 0) {
            ok = false;
            break;
        }
    }

    av_write_trailer(muxer);
    if (muxer->pb) {
        avio_closep(&muxer->pb);
    }
    avformat_free_context(muxer);
    return ok;
}

struct DecodedAudio
{
    std::vector<float> channels[2]; // planar, one vector per channel
    int sampleRate = 0;
    int channelCount = 0;
    bool formatOk = true;
    long frameCount = 0;
};

// Collects decoded frames the same way NdiSink::onDecodedAudio does: anything that is not
// planar float would be discarded by production, so count it as a failure here.
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

double rmsDbfs(const std::vector<float> &samples)
{
    if (samples.empty()) {
        return -300.0;
    }
    double energy = 0.0;
    for (const float sample : samples) {
        energy += double(sample) * double(sample);
    }
    const double rms = std::sqrt(energy / double(samples.size()));
    return rms > 0.0 ? 20.0 * std::log10(rms) : -300.0;
}

} // namespace


int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString whepUrl = argc > 1 ? QString::fromUtf8(argv[1])
                                     : QStringLiteral("http://172.19.30.21:8889/eriksobs/whep");
    const int captureSeconds = argc > 2 ? QString::fromUtf8(argv[2]).toInt() : 10;
    const QString outputDir = argc > 3 ? QString::fromUtf8(argv[3]) : QDir::currentPath();
    const QString username = argc > 4 ? QString::fromUtf8(argv[4]) : QString {};
    const QString password = argc > 5 ? QString::fromUtf8(argv[5]) : QString {};

    std::printf("C-Bridge live WebRTC audio smoke test\n");
    std::printf("WHEP endpoint: %s\n", qUtf8Printable(whepUrl));
    std::printf("capture window: %d s, output dir: %s\n\n", captureSeconds,
                qUtf8Printable(outputDir));

    CBridge::StreamConfig config;
    config.id = u"audio-smoke"_s;
    config.name = u"Audio smoke test"_s;
    config.whepUrl = QUrl(whepUrl);
    config.username = username;
    config.audioEnabled = true;

    std::vector<CapturedPacket> packets;
    std::vector<RawRtpPacket> rawPackets; // wire-level datagrams, for padding diagnostics
    RtpWireStats wireStats;
    std::mutex captureMutex;
    std::atomic<bool> capturing { true };
    std::atomic<quint64> videoFrames { 0 };
    std::atomic<bool> everRunning { false };
    QString lastError;

    CBridge::WebRtcSource source;
    source.setConfig(config);
    if (!password.isEmpty()) {
        source.setPassword(password);
    }

    QObject::connect(&source, &CBridge::WebRtcSource::stateChanged, [&](CBridge::StreamState state) {
        std::printf("[state] %s\n", stateName(state));
        if (state == CBridge::StreamState::Running) {
            everRunning = true;
        }
    });
    QObject::connect(&source, &CBridge::WebRtcSource::errorOccurred, [&](const QString &message) {
        std::printf("[error] %s\n", qUtf8Printable(message));
        lastError = message;
    });
    QObject::connect(&source, &CBridge::WebRtcSource::videoCodecNegotiated,
                     [](CBridge::VideoCodec codec) {
                         std::printf("[negotiated video] %s\n", videoCodecName(codec));
                     });

    source.setAudioCallback([&](const std::uint8_t *data, std::size_t size, quint32 rtpTimestamp) {
        if (!capturing || !data || size == 0) {
            return;
        }
        std::lock_guard lock(captureMutex);
        packets.push_back(CapturedPacket { std::vector<std::uint8_t>(data, data + size),
                                           std::int64_t(rtpTimestamp) });
    });
    source.setRawAudioPacketCallback([&](const std::uint8_t *data, std::size_t size) {
        if (!capturing || !data || size == 0) {
            return;
        }
        RawRtpPacket raw { std::vector<std::uint8_t>(data, data + size), false, 0, 0 };
        std::lock_guard lock(captureMutex);
        parseRtpHeader(raw, &wireStats);
        rawPackets.push_back(std::move(raw));
    });
    source.setVideoCallback([&](const std::uint8_t *, std::size_t, quint32) {
        if (capturing) {
            ++videoFrames;
        }
    });

    // Bail out early when the connection fails before any media has flowed.
    QObject::connect(&source, &CBridge::WebRtcSource::stateChanged, [&](CBridge::StreamState state) {
        if ((state == CBridge::StreamState::Failed || state == CBridge::StreamState::Idle)
            && !everRunning.load()) {
            QTimer::singleShot(1500, &app, [&] { app.quit(); });
        }
    });

    QTimer progressTimer;
    QObject::connect(&progressTimer, &QTimer::timeout, [&] {
        std::lock_guard lock(captureMutex);
        std::printf("  ... %zu audio packets, %llu video frames\n", packets.size(),
                    (unsigned long long)videoFrames.load());
    });

    QTimer captureTimer;
    QObject::connect(&captureTimer, &QTimer::timeout, [&] { app.quit(); });

    source.start();
    progressTimer.start(1000);
    captureTimer.setSingleShot(true);
    captureTimer.start(captureSeconds * 1000);

    app.exec();

    capturing = false;
    source.stop();

    // Let the WHEP DELETE and peer teardown finish before we go quiet.
    {
        QEventLoop loop;
        QTimer::singleShot(500, &loop, [&] { loop.quit(); });
        loop.exec();
    }


    std::lock_guard captureLock(captureMutex);

    if (packets.empty()) {
        std::printf("\nFAILED: no audio packets received from the WHEP endpoint.\n");
        if (!lastError.isEmpty()) {
            std::printf("Last error: %s\n", qUtf8Printable(lastError));
        }
        std::printf("Check that the source actually has an audio track and that this host can "
                    "reach the endpoint (UDP for ICE).\n");
        return 1;
    }

    // Capture statistics.
    std::uint64_t totalBytes = 0;
    auto minTs = packets.front().rtpTimestamp;
    auto maxTs = packets.front().rtpTimestamp;
    for (const auto &packet : packets) {
        totalBytes += packet.data.size();
        minTs = std::min(minTs, packet.rtpTimestamp);
        maxTs = std::max(maxTs, packet.rtpTimestamp);
    }
    const double durationSeconds = double(maxTs - minTs) / 48000.0;

    std::printf("\n== Capture summary ==\n");
    std::printf("audio packets: %zu (%llu bytes)\n", packets.size(), (unsigned long long)totalBytes);
    std::printf("video frames:  %llu\n", (unsigned long long)videoFrames.load());
    if (durationSeconds > 0.0) {
        std::printf("span: %.2f s, audio bitrate ~%.1f kbps\n", durationSeconds,
                    double(totalBytes) * 8.0 / durationSeconds / 1000.0);
    }

    // Wire-level RTP diagnostics: verify that RFC 3550 padding is stripped before depacketization.
    std::printf("rtp datagrams: %llu total, %llu padded (%llu bytes stripped)\n",
                (unsigned long long)wireStats.totalPackets, (unsigned long long)wireStats.paddedPackets,
                (unsigned long long)wireStats.strippedBytes);
    if (wireStats.paddedPackets > 0) {
        std::printf("pad counts: min=%d max=%d", wireStats.minPadCount, wireStats.maxPadCount);
        for (const auto &entry : wireStats.padHistogram) {
            std::printf(", %dB x%llu", entry.first, (unsigned long long)entry.second);
        }
        std::printf("\n");
    }
    if (rawPackets.size() != packets.size()) {
        std::printf("note: %zu raw datagrams vs %zu depacketized payloads (%zu dropped)\n", rawPackets.size(),
                    packets.size(), rawPackets.size() - packets.size());
    }

    // Raw wire dump for offline byte-level analysis: [uint32 LE size][full RTP datagram].
    {
        const QString rtpPath = outputDir + u"/webrtc_audio_rtp.raw"_s;
        std::ofstream rtp(rtpPath.toUtf8().constData(), std::ios::binary);
        if (rtp) {
            for (const auto &raw : rawPackets) {
                const std::uint32_t size = static_cast<std::uint32_t>(raw.data.size());
                rtp.write(reinterpret_cast<const char *>(&size), sizeof(size));
                rtp.write(reinterpret_cast<const char *>(raw.data.data()), size);
            }
            std::printf("rtp dump: %s\n", qUtf8Printable(rtpPath));
        }
    }

    // --- Per-packet diagnostics -------------------------------------------------
    // Decodes each packet with a fresh decoder (Opus packets are independent) to find which
    // payloads are not valid Opus at all, and dumps size/TOC/timestamp patterns for analysis.
    {
        const QString csvPath = outputDir + u"/webrtc_audio_packets.csv"_s;
        std::ofstream csv(csvPath.toUtf8().constData());
        if (csv) {
            csv << "idx,rtp_ts,size,toc,valid\n";
            long validCount = 0;
            for (std::size_t i = 0; i < packets.size(); ++i) {
                const auto &p = packets[i];
                bool valid = false;
                if (!p.data.empty()) {
                    CBridge::AudioDecoder probe;
                    QString pe;
                    if (probe.open(48000, 2, &pe)) {
                        valid = probe.decode(p.data.data(), p.data.size(), 0, &pe) && pe.isEmpty();
                    }
                }
                if (valid) {
                    ++validCount;
                }
                csv << i << ',' << p.rtpTimestamp << ',' << p.data.size() << ','
                    << (p.data.empty() ? -1 : int(p.data[0])) << ',' << (valid ? 1 : 0) << '\n';
            }
            std::printf("per-packet probe: %ld/%zu valid, csv: %s\n", validCount, packets.size(),
                        qUtf8Printable(csvPath));
        } else {
            std::printf("could not write the packet CSV\n");
        }

        // Raw dump for offline byte-level analysis: [uint32 LE rtpTs][uint32 LE size][payload].
        const QString rawPath = outputDir + u"/webrtc_audio_packets.raw"_s;
        std::ofstream raw(rawPath.toUtf8().constData(), std::ios::binary);
        if (raw) {
            for (const auto &p : packets) {
                const std::uint32_t ts = static_cast<std::uint32_t>(p.rtpTimestamp);
                const std::uint32_t size = static_cast<std::uint32_t>(p.data.size());
                raw.write(reinterpret_cast<const char *>(&ts), sizeof(ts));
                raw.write(reinterpret_cast<const char *>(&size), sizeof(size));
                if (size > 0) {
                    raw.write(reinterpret_cast<const char *>(p.data.data()), size);
                }
            }
            std::printf("raw dump: %s\n", qUtf8Printable(rawPath));
        }
    }

    // Decode with the production decoder exactly like NdiSink does (opened at the StreamFormat
    // defaults of 48 kHz / stereo; the decoder follows whatever the stream actually is).
    CBridge::AudioDecoder decoder;
    QString error;
    if (!decoder.open(48000, 2, &error)) {
        std::printf("\nFAILED: AudioDecoder::open failed: %s\n", qUtf8Printable(error));
        return 1;
    }

    DecodedAudio decoded;
    decoder.setFrameCallback([&decoded](AVFrame *frame) { collectFrames(frame, decoded); });

    long decodeErrors = 0;
    for (const auto &packet : packets) {
        if (!decoder.decode(packet.data.data(), packet.data.size(), packet.rtpTimestamp, &error)) {
            ++decodeErrors;
            if (decodeErrors <= 3) {
                std::printf("decode error: %s\n", qUtf8Printable(error));
            }
        }
    }

    const bool allFltp = decoded.formatOk && decodeErrors == 0;
    const int channelCount = decoded.channelCount > 0 ? decoded.channelCount : 1;
    const int sampleRate = decoded.sampleRate > 0 ? decoded.sampleRate : 48000;

    std::printf("\n== Decode results (production AudioDecoder) ==\n");
    std::printf("decoder: %s\n", qUtf8Printable(decoder.decoderName()));
    std::printf("decoded frames: %ld, decode errors: %ld\n", decoded.frameCount, decodeErrors);
    if (decoder.trailingByteRetryCount() > 0) {
        std::printf(
            "trailing-byte fallback used for %llu packets - the upstream encoder appends one "
            "undeclared extra byte to its Opus payloads; fix the source stream\n",
            (unsigned long long)decoder.trailingByteRetryCount());
    }
    if (decoded.channelCount > 0) {
        std::printf("format: %s rate=%d ch=%d\n", allFltp ? "FLTP" : "MIXED/NON-FLTP", sampleRate,
                    channelCount);
    } else {
        std::printf("no frames decoded at all\n");
    }

    bool audible = false;
    for (int c = 0; c < channelCount && c < 2; ++c) {
        const double level = rmsDbfs(decoded.channels[c]);
        std::printf("channel %d: %.1f dBFS RMS over %zu samples\n", c, level,
                    decoded.channels[c].size());
        audible = audible || (level > -40.0);
    }

    // Artifacts for manual verification.
    if (decoded.channelCount > 0) {
        const QString wavPath = outputDir + u"/webrtc_audio_capture.wav"_s;
        if (writePcm16Wav(wavPath, std::vector<std::vector<float>> { decoded.channels[0],
                                                                     decoded.channels[1] },
                          sampleRate)) {
            std::printf("\nWAV written: %s\n", qUtf8Printable(wavPath));
        } else {
            std::printf("could not write the WAV file\n");
        }

        const QString tsPath = outputDir + u"/webrtc_audio_capture.ts"_s;
        std::uint64_t trimmedPackets = 0;
        std::uint64_t trimmedBytes = 0;
        if (writeTsFile(tsPath, packets, sampleRate, channelCount, &trimmedPackets, &trimmedBytes)) {
            std::printf("TS written:  %s (play in VLC to check what multicast delivers)\n",
                        qUtf8Printable(tsPath));
            if (trimmedPackets > 0) {
                std::printf(
                    "sanitizer stripped trailing bytes from %llu/%zu packets (%llu bytes) before "
                    "muxing, matching StreamPipeline\n",
                    (unsigned long long)trimmedPackets, packets.size(),
                    (unsigned long long)trimmedBytes);
            }
        } else {
            std::printf("could not write the TS file\n");
        }
    }

    const bool pass = allFltp && decoded.channelCount > 0 && sampleRate == 48000 && audible;
    std::printf("\n%s\n", pass ? "LIVE AUDIO TEST PASSED" : "LIVE AUDIO TEST FAILED");
    if (!pass) {
        if (!allFltp) {
            std::printf(" - frames were not all planar float (NDI would discard them)\n");
        }
        if (sampleRate != 48000) {
            std::printf(" - unexpected sample rate %d\n", sampleRate);
        }
        if (!audible) {
            std::printf(" - decoded audio is silent or below -40 dBFS\n");
        }
    }
    return pass ? 0 : 1;
}

