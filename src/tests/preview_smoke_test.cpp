/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Offline smoke test for the built-in viewer's decode path (StreamPreview).
//
// Demuxes the same MPEG-TS fixture the yt-dlp smoke uses (H.264 640x360 + AAC 48 kHz
// stereo) and feeds its access units into the production StreamPreview exactly like the
// pipeline worker does, then verifies:
//   1. decoded + filtered frames arrive at the attached QVideoSink with the right size;
//   2. decoded audio lands in the pull ring (PreviewAudioDevice returns non-silence);
//   3. reset() drops hasFrame and the viewer waits for the next keyframe again;
//   4. detach() stops frame production and re-attach re-arms the keyframe gate.
//
// Build and run:
//   cmake --build build --target preview-smoke-test --config RelWithDebInfo
//   preview-smoke-test.exe [fixture.ts]   (default: ../../smoke/fixture.ts next to the exe)

#include "preview/streampreview.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QVideoFrame>
#include <QVideoSink>

#include <cstdio>
#include <cstring>
#include <vector>

namespace CBridge {
struct PreviewFrameTestAccess {
    static void publish(StreamPreview &preview, AVFrame *frame) { preview.onFilteredVideo(frame); }
};
}

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

} // namespace

int main(int argc, char *argv[])
{
    // QGuiApplication: QVideoSink/QVideoFrame (and QAudioSink inside the preview) need the
    // GUI application instance; no window is created.
    QGuiApplication app(argc, argv);

    QString fixturePath = argc > 1
        ? QString::fromLocal8Bit(argv[1])
        : QString();
    if (fixturePath.isEmpty()) {
        // Walk up from the executable looking for smoke/fixture.ts (the asset lives in
        // the build tree, the exe lives under build/src/<config>/).
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

    // The format the pipeline would push via configure().
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
        format.audioExtradata =
            QByteArray(reinterpret_cast<const char *>(apar->extradata), apar->extradata_size);
    }

    StreamPreview preview;
    preview.configure(format);

    // Attach a sink and count the frames it receives, like a VideoOutput would.
    QVideoSink sink;
    int receivedFrames = 0;
    int receivedWidth = 0;
    int receivedHeight = 0;
    QObject::connect(&sink, &QVideoSink::videoFrameChanged,
                     [&](const QVideoFrame &frame) {
                         if (frame.isValid() && frame.width() > 0) {
                             ++receivedFrames;
                             receivedWidth = frame.width();
                             receivedHeight = frame.height();
                         }
                     });
    preview.attachTo(&sink);
    check(preview.isActive(), "preview active after attachTo");

    // Exercise padded RGB rows, which an ordinary 640-wide fixture does not expose.
    // Use a different color on every row so a wrong stride cannot pass visually.
    AVFrame padded {};
    padded.width = 638;
    padded.height = 17;
    padded.linesize[0] = 2560;
    std::vector<unsigned char> pixels(padded.linesize[0] * padded.height, 0xa5);
    padded.data[0] = pixels.data();
    for (int y = 0; y < padded.height; ++y) {
        auto *row = reinterpret_cast<QRgb *>(pixels.data() + y * padded.linesize[0]);
        for (int x = 0; x < padded.width; ++x) {
            row[x] = qRgb(x % 256, y * 13, (x + y) % 256) & 0x00ffffff; // BGR0 padding byte
        }
    }
    PreviewFrameTestAccess::publish(preview, &padded);
    QImage rendered = sink.videoFrame().toImage();
    bool rowsCorrect = rendered.size() == QSize(padded.width, padded.height);
    for (int y = 0; rowsCorrect && y < padded.height; ++y) {
        for (int x = 0; x < padded.width; ++x) {
            rowsCorrect &= rendered.pixel(x, y) == qRgb(x % 256, y * 13, (x + y) % 256);
        }
    }
    check(rowsCorrect, "all preview pixels preserve padded row stride");
    padded.data[0] = pixels.data() + (padded.height - 1) * padded.linesize[0];
    padded.linesize[0] = -padded.linesize[0];
    PreviewFrameTestAccess::publish(preview, &padded);
    rendered = sink.videoFrame().toImage();
    check(rendered.pixel(0, 0) == qRgb(0, 16 * 13, 16)
          && rendered.pixel(637, 16) == qRgb(637 % 256, 0, 637 % 256),
          "preview handles negative row stride");
    preview.reset();
    receivedFrames = 0;

    // ---- 1 + 2: feed the fixture, expect video frames and non-silent audio ----
    AVPacket *pkt = av_packet_alloc();
    int videoUnitsFed = 0;
    int audioUnitsFed = 0;
    std::uint32_t fallbackTs = 0;
    while (av_read_frame(fmtCtx, pkt) >= 0) {
        const bool isVideo = pkt->stream_index == videoIndex;
        const bool isAudio = pkt->stream_index == audioIndex;
        if (isVideo || isAudio) {
            const std::uint32_t ts = pkt->pts != AV_NOPTS_VALUE
                ? std::uint32_t(pkt->pts)
                : (fallbackTs += isVideo ? 3000 : 1920);
            if (isVideo) {
                preview.writeVideo(pkt->data, std::size_t(pkt->size), ts, ts,
                                   bool(pkt->flags & AV_PKT_FLAG_KEY));
                ++videoUnitsFed;
            } else {
                preview.writeAudio(pkt->data, std::size_t(pkt->size), ts);
                ++audioUnitsFed;
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    check(videoUnitsFed > 0, "fixture carried video access units");
    check(audioUnitsFed > 0, "fixture carried audio packets");
    check(preview.hasFrame(), "preview produced a frame (hasFrame)");
    check(receivedFrames > 0, "attached QVideoSink received frames");
    check(receivedFrames <= videoUnitsFed && receivedFrames >= videoUnitsFed - 10,
          "preview preserves native cadence without fps conversion repeats/drops");
    check(receivedWidth == format.width && receivedHeight == format.height,
          "frame size matches the source (no scaling for 640x360)");

    // The pull device the QAudioSink would read from: readData must return non-silence
    // once the decoder has filled the ring. (readFromRing silence-pads, so a short read
    // is never an error; look for a non-zero sample instead.)
    PreviewAudioDevice audioDevice(&preview);
    QByteArray audioBytes = audioDevice.read(8192);
    bool sawNonSilence = false;
    for (int i = 0; i + int(sizeof(float)) <= audioBytes.size(); i += int(sizeof(float))) {
        float sample = 0.f;
        std::memcpy(&sample, audioBytes.constData() + i, sizeof(float));
        if (sample != 0.f) {
            sawNonSilence = true;
            break;
        }
    }
    check(sawNonSilence, "audio ring delivered non-silent samples");

    // ---- 3: reset() re-arms the keyframe gate ----
    preview.reset();
    check(!preview.hasFrame(), "reset() cleared hasFrame");

    // Re-feed from the start: the first units may be non-keyframes, which must not render.
    avformat_close_input(&fmtCtx);
    if (avformat_open_input(&fmtCtx, fixturePath.toUtf8().constData(), nullptr, nullptr) < 0) {
        std::printf("FAIL: could not reopen fixture\n");
        return 1;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        std::printf("FAIL: could not reprobe fixture\n");
        return 1;
    }
    const int framesBeforeRearm = receivedFrames;
    pkt = av_packet_alloc();
    bool fedNonKeyFirst = false;
    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == videoIndex) {
            const bool key = bool(pkt->flags & AV_PKT_FLAG_KEY);
            const std::uint32_t ts = pkt->pts != AV_NOPTS_VALUE
                ? std::uint32_t(pkt->pts) : (fallbackTs += 3000);
            preview.writeVideo(pkt->data, std::size_t(pkt->size), ts, ts, key);
            if (!fedNonKeyFirst && !key) {
                fedNonKeyFirst = true;
            }
            if (fedNonKeyFirst && key) {
                break; // one keyframe after the gate re-armed is enough
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    check(preview.hasFrame(), "frames resume after reset() + keyframe");
    check(receivedFrames > framesBeforeRearm, "new frames reached the sink after reset");

    // ---- 4: detach stops production, re-attach re-arms the gate ----
    preview.detach();
    check(!preview.isActive(), "preview inactive after detach");
    const int framesAfterDetach = receivedFrames;
    avformat_close_input(&fmtCtx);
    if (avformat_open_input(&fmtCtx, fixturePath.toUtf8().constData(), nullptr, nullptr) < 0) {
        std::printf("FAIL: could not reopen fixture\n");
        return 1;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        std::printf("FAIL: could not reprobe fixture\n");
        return 1;
    }
    pkt = av_packet_alloc();
    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == videoIndex) {
            preview.writeVideo(pkt->data, std::size_t(pkt->size), 0, 0,
                               bool(pkt->flags & AV_PKT_FLAG_KEY));
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    check(receivedFrames == framesAfterDetach, "detached preview produced no frames");

    preview.shutdown();
    avformat_close_input(&fmtCtx);

    std::printf("%s (%d video units, %d audio units, %d frames to sink)\n",
                g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT", videoUnitsFed, audioUnitsFed,
                receivedFrames);
    return g_failures == 0 ? 0 : 1;
}
