/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Regression smoke test for the TS multicast sink's timestamp handling (the B-frame fix).
//
// Feeds a real B-frame MPEG-TS fixture (H.264 with IBBP reordering + AAC 48 kHz) through
// the production TsMulticastSink over loopback, captures the emitted UDP datagrams and
// demuxes them again to verify:
//   1. the output DTS is strictly monotonically increasing in decode order (the muxer's
//      contract — a violation would make FFmpeg reject every packet with EINVAL);
//   2. every packet's PTS/DTS pair matches the input's presentation/decode timeline exactly,
//      i.e. B-frame presentation times survive instead of being collapsed into DTS — which
//      is what used to make downstream player clocks jump back and forth;
//   3. the fixture actually carries B-frames (pts != dts on some packets), so the test keeps
//      meaning if the asset ever changes.
//
// Build and run:
//   cmake --build build --target ts-smoke-test --config RelWithDebInfo
//   ts-smoke-test.exe [fixture.ts]   (default: ../../smoke/fixture.ts next to the exe)

#include "sinks/tsmulticastsink.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUdpSocket>
#include <QNetworkDatagram>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Qt::Literals::StringLiterals;
using namespace CBridge;

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!ok) {
        ++g_failures;
    }
}

/// One video packet's clocks on the 90 kHz grid, absolute (not yet rebased).
struct ClockPair {
    qint64 dts = -1;
    qint64 pts = -1;
};

/// Memory-backed AVIO so the captured multicast bytes can be demuxed in place.
struct MemoryReader {
    const std::uint8_t *data;
    std::size_t size;
    std::size_t pos = 0;
};

int memoryRead(void *opaque, uint8_t *buf, int bufSize)
{
    auto *reader = static_cast<MemoryReader *>(opaque);
    const std::size_t remaining = reader->size - reader->pos;
    if (remaining == 0) {
        return 0; // 0 is a clean EOF for libavformat (a negative value would be an I/O error)
    }
    const int n = int(std::min<std::size_t>(std::size_t(bufSize), remaining));
    std::memcpy(buf, reader->data + reader->pos, std::size_t(n));
    reader->pos += std::size_t(n);
    return n;
}

/// Demuxes a captured MPEG-TS byte stream and returns the video packets' clocks plus how
/// many audio packets it carried. Returns false when the capture is not demuxable TS.
bool demuxCaptured(const std::vector<std::uint8_t> &bytes, std::vector<ClockPair> *videoPairs,
                   int *audioCount)
{
    MemoryReader reader { bytes.data(), bytes.size() };
    // The buffer must come from av_malloc, not malloc: the mpegts demuxer's ffio_ensure_seekback()
    // replaces s->buffer with an av_malloc'd one and av_free()s ours (see YtdlpSource::start for
    // the same reasoning). A buffer from the app's CRT heap would be freed by FFmpeg's CRT — a
    // cross-heap free that corrupts memory.
    AVIOContext *io = avio_alloc_context(static_cast<uint8_t *>(av_malloc(65536)), 65536, 0, &reader,
                                         &memoryRead, nullptr, nullptr);
    if (!io) {
        return false;
    }

    AVFormatContext *ctx = avformat_alloc_context();
    bool ok = false;
    if (ctx) {
        ctx->pb = io;
        // Force the mpegts demuxer: the capture starts with the muxer's own PAT/PMT, but an
        // explicit probe never depends on byte offsets.
        const AVInputFormat *mpegts = av_find_input_format("mpegts");
        if (avformat_open_input(&ctx, "", mpegts, nullptr) == 0
            && avformat_find_stream_info(ctx, nullptr) >= 0) {
            int videoIndex = -1;
            for (unsigned i = 0; i < ctx->nb_streams; ++i) {
                if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    videoIndex = int(i);
                    break;
                }
            }

            AVPacket *packet = av_packet_alloc();
            ok = packet != nullptr && videoIndex >= 0;
            while (ok && av_read_frame(ctx, packet) >= 0) {
                if (packet->stream_index == videoIndex) {
                    const qint64 dts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
                    const qint64 pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : dts;
                    videoPairs->push_back(ClockPair { dts, pts });
                } else if (audioCount) {
                    ++*audioCount;
                }
                av_packet_unref(packet);
            }
            if (packet) {
                av_packet_free(&packet);
            }
        }
    }

    // CUSTOM_IO: the context owns neither pb nor its buffer.
    if (ctx) {
        ctx->pb = nullptr;
        avformat_free_context(ctx);
    }
    av_freep(&io->buffer);
    avio_context_free(&io);
    return ok;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QString fixturePath = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (fixturePath.isEmpty()) {
        // Walk up from the executable looking for smoke/fixture.ts (the asset lives in the
        // build tree, the exe lives under build/bin/<config>/).
        QDir dir(QCoreApplication::applicationDirPath());
        for (int up = 0; up < 5 && fixturePath.isEmpty(); ++up) {
            const QString candidate = dir.filePath(u"smoke/fixture.ts"_s);
            if (QFileInfo::exists(candidate)) {
                fixturePath = candidate;
                break;
            }
            if (!dir.cdUp()) {
                break;
            }
        }
    }
    if (fixturePath.isEmpty()) {
        std::printf("FAIL: fixture.ts not found (pass its path as argv[1])\n");
        return 1;
    }

    AVFormatContext *fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, fixturePath.toUtf8().constData(), nullptr, nullptr) < 0) {
        std::printf("FAIL: could not open %s\n", qPrintable(fixturePath));
        return 1;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        std::printf("FAIL: could not probe %s\n", qPrintable(fixturePath));
        avformat_close_input(&fmtCtx);
        return 1;
    }

    int videoIndex = -1;
    int audioIndex = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        const AVCodecParameters *par = fmtCtx->streams[i]->codecpar;
        if (videoIndex < 0 && par->codec_type == AVMEDIA_TYPE_VIDEO
            && par->codec_id == AV_CODEC_ID_H264) {
            videoIndex = int(i);
        } else if (audioIndex < 0 && par->codec_type == AVMEDIA_TYPE_AUDIO
                   && (par->codec_id == AV_CODEC_ID_AAC || par->codec_id == AV_CODEC_ID_AAC_LATM)) {
            audioIndex = int(i);
        }
    }
    if (videoIndex < 0 || audioIndex < 0) {
        std::printf("FAIL: fixture must carry H.264 video and AAC audio\n");
        avformat_close_input(&fmtCtx);
        return 1;
    }

    // The format the pipeline would push via configure(). Video extradata is left empty on
    // purpose: TS keyframes carry SPS/PPS in-band, which is all the mpegts muxer needs.
    StreamFormat format;
    format.videoCodec = VideoCodec::H264;
    format.width = fmtCtx->streams[videoIndex]->codecpar->width;
    format.height = fmtCtx->streams[videoIndex]->codecpar->height;
    format.hasAudio = true;
    format.audioCodec = fmtCtx->streams[audioIndex]->codecpar->codec_id == AV_CODEC_ID_AAC_LATM
        ? AudioCodec::AacLatm
        : AudioCodec::Aac;
    format.audioSampleRate = fmtCtx->streams[audioIndex]->codecpar->sample_rate;
    format.audioChannels = fmtCtx->streams[audioIndex]->codecpar->ch_layout.nb_channels;
    if (const AVCodecParameters *apar = fmtCtx->streams[audioIndex]->codecpar;
        apar->extradata_size > 0) {
        format.audioExtradata = QByteArray(reinterpret_cast<const char *>(apar->extradata),
                                           apar->extradata_size);
    }

    // Loopback unicast stands in for the multicast group (the udp:// muxer accepts any IP).
    // The capture socket binds before open() so it also sees the first PAT/PMT.
    const quint16 port = 5099;
    QUdpSocket capture;
    if (!capture.bind(QHostAddress::LocalHost, port)) {
        std::printf("FAIL: could not bind the capture socket\n");
        avformat_close_input(&fmtCtx);
        return 1;
    }

    TsMulticastSinkConfig config;
    config.groupAddress = u"127.0.0.1"_s;
    config.port = port;
    config.ttl = 1;
    TsMulticastSink sink(config);

    QString error;
    if (!sink.open(format, &error)) {
        std::printf("FAIL: open: %s\n", qPrintable(error));
        avformat_close_input(&fmtCtx);
        return 1;
    }
    check(true, "sink opened");

    // --- Feed the fixture exactly like a source would ---------------------------------
    const AVRational videoClock { 1, 90000 };
    std::vector<ClockPair> inputPairs; // absolute 90 kHz ticks, decode order
    int fedVideo = 0;
    int fedAudio = 0;
    bool sawBFrame = false;

    // The feed runs faster than real time, so the capture socket must be drained while
    // feeding — otherwise the loopback receive queue overflows and datagrams are lost.
    // Delivery is asynchronous, so a plain non-blocking drain cannot keep up: after each
    // small batch we wait until the socket has been quiet for a moment instead.
    std::vector<std::uint8_t> captured;
    auto drainNow = [&]() {
        while (capture.hasPendingDatagrams()) {
            const QNetworkDatagram datagram = capture.receiveDatagram();
            const QByteArray bytes = datagram.data();
            captured.insert(captured.end(), bytes.constData(), bytes.constData() + bytes.size());
        }
    };
    auto drainUntilQuiet = [&]() {
        int quietMs = 0;
        while (quietMs < 20) {
            if (capture.waitForReadyRead(10)) {
                drainNow();
                quietMs = 0;
            } else {
                quietMs += 10;
            }
        }
    };

    AVPacket *packet = av_packet_alloc();
    int sinceDrain = 0;
    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index == videoIndex && packet->size > 0) {
            const qint64 dts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
            const qint64 pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : dts;
            if (dts >= 0 && pts >= 0) {
                const ClockPair pair { av_rescale_q(dts, fmtCtx->streams[videoIndex]->time_base, videoClock),
                                       av_rescale_q(pts, fmtCtx->streams[videoIndex]->time_base, videoClock) };
                if (pair.pts != pair.dts) {
                    sawBFrame = true; // pts ahead of dts: a B-frame follows this packet
                }
                inputPairs.push_back(pair);
                sink.writeVideo(packet->data, std::size_t(packet->size), quint32(pair.dts),
                                quint32(pair.pts), bool(packet->flags & AV_PKT_FLAG_KEY));
                ++fedVideo;
            }
        } else if (packet->stream_index == audioIndex && packet->size > 0) {
            const qint64 dts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
            if (dts >= 0) {
                const int rate = format.audioSampleRate > 0 ? format.audioSampleRate : 48000;
                const qint64 ts = av_rescale_q(dts, fmtCtx->streams[audioIndex]->time_base, AVRational { 1, rate });
                sink.writeAudio(packet->data, std::size_t(packet->size), quint32(ts));
                ++fedAudio;
            }
        }
        if (++sinceDrain >= 32) {
            sinceDrain = 0;
            drainUntilQuiet(); // bound the in-flight backlog so nothing is dropped
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);

    check(sawBFrame, "fixture carries B-frames (pts != dts on some packets)");
    check(fedVideo > 0 && fedAudio > 0, "fed video and audio units to the sink");

    // --- Drain until the pipe is quiet --------------------------------------------------
    int quietMs = 0;
    while (quietMs < 300) {
        if (capture.waitForReadyRead(50)) {
            drainNow();
            quietMs = 0;
        } else {
            quietMs += 50;
        }
    }

    sink.close();
    avformat_close_input(&fmtCtx);

    check(!captured.empty(), "datagrams arrived on the capture socket");
    if (captured.empty()) {
        std::printf("ts-smoke-test: %d failure(s)\n", g_failures);
        return 1;
    }

    // --- Demux what came out and compare clocks ----------------------------------------
    char sizeLine[128];
    std::snprintf(sizeLine, sizeof(sizeLine), "capture holds %zu bytes (%.0f TS packets)", captured.size(),
                  double(captured.size()) / 188.0);
    std::printf("info: %s\n", sizeLine);

    // TS_SMOKE_DUMP=path writes the raw capture for offline inspection with ffprobe.
    const char *dumpEnv = std::getenv("TS_SMOKE_DUMP");
    if (dumpEnv && dumpEnv[0]) {
        QFile dump(QString::fromLocal8Bit(dumpEnv));
        if (dump.open(QIODevice::WriteOnly)) {
            dump.write(reinterpret_cast<const char *>(captured.data()), qint64(captured.size()));
            std::printf("info: capture dumped to %s\n", dumpEnv);
        }
    }

    std::vector<ClockPair> outputPairs;
    int capturedAudio = 0;
    check(demuxCaptured(captured, &outputPairs, &capturedAudio), "capture demuxes as MPEG-TS");

    char countLine[128];
    std::snprintf(countLine, sizeof(countLine), "every video packet survived the round trip (%zu of %d)",
                  outputPairs.size(), fedVideo);
    check(int(outputPairs.size()) == fedVideo, countLine);
    check(capturedAudio > 0, "audio packets survived the round trip");

    if (int(outputPairs.size()) != int(inputPairs.size())) {
        std::printf("ts-smoke-test: %d failure(s)\n", g_failures);
        return 1;
    }

    // Rebase both lists to their first packet's DTS, then compare pair by pair. The sink
    // rebases to its own base and the mpegts muxer writes PTS/DTS verbatim into the PES
    // headers, so exact equality is expected for a single uninterrupted feed.
    const qint64 inBase = inputPairs.front().dts;
    const qint64 outBase = outputPairs.front().dts;

    bool dtsMonotonic = true;
    bool pairsMatch = true;
    qint64 lastOutDts = -1;
    for (std::size_t i = 0; i < inputPairs.size(); ++i) {
        const qint64 inDts = inputPairs[i].dts - inBase;
        const qint64 inPts = inputPairs[i].pts - inBase;
        const qint64 outDts = outputPairs[i].dts - outBase;
        const qint64 outPts = outputPairs[i].pts - outBase;

        if (outDts <= lastOutDts) {
            dtsMonotonic = false; // the muxer's contract: decode-order DTS must advance
        }
        lastOutDts = outDts;
        if (inDts != outDts || inPts != outPts) {
            pairsMatch = false;
        }
    }

    check(dtsMonotonic, "output DTS is strictly monotonic in decode order");
    check(pairsMatch, "every PTS/DTS pair matches the input timeline (B-frame times intact)");

    std::printf("ts-smoke-test: %s (%d video, %d audio)\n", g_failures == 0 ? "all checks passed" : "FAILED",
                fedVideo, fedAudio);
    return g_failures == 0 ? 0 : 1;
}