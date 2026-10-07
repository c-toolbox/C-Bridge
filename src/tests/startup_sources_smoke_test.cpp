// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual configured sources and preview decoder, without changing settings.
#include "config/bridgeconfig.h"
#include "media/bitstreaminspector.h"
#include "media/opuspayloadsanitizer.h"
#include "preview/streampreview.h"
#include "srt/srtsource.h"
#include "ytdlp/ytdlpsource.h"

#include <QGuiApplication>
#include <QTimer>
#include <QVideoSink>
#include <atomic>
#include <chrono>
#include <cmath>
#include <QVideoFrame>
#include <cstdio>
#include <memory>
#include <vector>

using namespace CBridge;
using Clock = std::chrono::steady_clock;

struct Probe {
    QString name;
    std::unique_ptr<StreamSource> source;
    StreamPreview preview;
    QVideoSink sink;
    BitstreamInspector inspector;
    std::atomic<int> units {0};
    std::atomic<int> frames {0};
    std::atomic<int> audio {0};
    std::atomic<int> width {0}, height {0};
    std::atomic<int> sourceWidth {0}, sourceHeight {0};
    Clock::time_point first {}, last {};
    quint32 firstDts = 0, lastDts = 0;
    bool configured = false;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 2) { std::puts("Usage: startup-sources-smoke-test config.json"); return 1; }
    QString error;
    auto config = BridgeConfig::load(QString::fromLocal8Bit(argv[1]), &error);
    if (!config) { std::fprintf(stderr, "%s\n", qPrintable(error)); return 1; }
    std::vector<std::unique_ptr<Probe>> probes;
    for (const auto &stream : config->streams) {
        if (!stream.enabled || (stream.sourceKind != SourceKind::Youtube && stream.sourceKind != SourceKind::Srt)) continue;
        auto probe = std::make_unique<Probe>();
        auto *p = probe.get();
        p->name = stream.name;
        if (stream.sourceKind == SourceKind::Youtube) p->source = std::make_unique<YtdlpSource>();
        else p->source = std::make_unique<SrtSource>();
        p->source->setConfig(stream);
        p->preview.setMuted(true);
        p->preview.attachTo(&p->sink);
        QObject::connect(&p->sink, &QVideoSink::videoFrameChanged, &app,
            [p](const QVideoFrame &frame) {
                if (frame.isValid()) { ++p->frames; p->width = frame.width(); p->height = frame.height(); }
            });
        QObject::connect(p->source.get(), &StreamSource::errorOccurred, &app,
            [p](const QString &message) { std::fprintf(stderr, "%s: %s\n", qPrintable(p->name), qPrintable(message)); });
        p->source->setVideoCallback([p](const std::uint8_t *data, std::size_t size, quint32 dts, quint32 pts) {
            if (!p->configured) {
                StreamFormat format;
                format.videoCodec = p->source->negotiatedVideoCodec();
                format.audioCodec = p->source->negotiatedAudioCodec();
                format.hasAudio = format.audioCodec != AudioCodec::Unknown;
                format.audioSampleRate = p->source->negotiatedAudioSampleRate();
                format.audioChannels = p->source->negotiatedAudioChannels();
                format.audioExtradata = p->source->negotiatedAudioExtradata();
                p->preview.configure(format);
                p->inspector.init(format.videoCodec);
                p->configured = true;
            }
            if (p->units == 0) { p->first = Clock::now(); p->firstDts = dts; }
            p->last = Clock::now(); p->lastDts = dts;
            ++p->units;
            const auto info = p->inspector.inspect(data, size);
            p->sourceWidth = info.width;
            p->sourceHeight = info.height;
            p->preview.writeVideo(data, size, dts, pts, info.isKeyframe);
        });
        p->source->setAudioCallback([p](const std::uint8_t *data, std::size_t size, quint32 ts) {
            ++p->audio;
            // Like StreamPipeline, wait for the first video keyframe to configure the
            // negotiated audio format before feeding packets into the preview decoder.
            if (!p->configured || !p->preview.hasFrame()) {
                return;
            }
            if (p->source->negotiatedAudioCodec() == AudioCodec::Opus) {
                size = OpusPayloadSanitizer::validPrefixLength(data, size);
            }
            p->preview.writeAudio(data, size, ts);
        });
        probes.push_back(std::move(probe));
    }
    for (auto &p : probes) p->source->start();
    // Internet extraction and initial buffering may exceed the offline fixture's budget.
    const int durationMs = argc > 2 ? std::max(1000, QString::fromLocal8Bit(argv[2]).toInt()) : 20000;
    QTimer::singleShot(durationMs, &app, &QCoreApplication::quit);
    app.exec();
    int failures = 0;
    for (auto &p : probes) {
        p->source->stop();
        const double wall = std::chrono::duration<double>(p->last - p->first).count();
        const double media = double(quint32(p->lastDts - p->firstDts)) / 90000.0;
        const bool expectedSize = argc < 5 || (p->sourceWidth == QString::fromLocal8Bit(argv[3]).toInt()
                                              && p->sourceHeight == QString::fromLocal8Bit(argv[4]).toInt());
        const bool passed = p->frames > 30 && wall > 1.0 && std::abs(wall - media) < 1.0 && expectedSize;
        std::printf("%s: %s: %d video packets (%dx%d), %d preview frames (%dx%d), %d audio packets; wall %.3fs, media %.3fs\n",
            passed ? "PASS" : "FAIL", qPrintable(p->name), p->units.load(), p->sourceWidth.load(),
            p->sourceHeight.load(), p->frames.load(), p->width.load(), p->height.load(), p->audio.load(), wall, media);
        failures += !passed;
        p->preview.shutdown();
    }
    return failures || probes.empty() ? 1 : 0;
}
