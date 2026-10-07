/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ytdlp/ytdlpsource.h"
#include "media/bitstreaminspector.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <atomic>
#include <cstdio>

using namespace CBridge;

// Real yt-dlp/FFmpeg integration test. Takes a URL, video+audio selector and
// expected coded dimensions; it deliberately exercises the source's control API.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto args = app.arguments();
    // A child that resolves correctly but fails before writing its first media byte
    // reproduces AVIO's historical infinite retry when the pipe callback returned 0.
    if (args.contains("-J")) {
        std::puts(R"({"duration":30,"vcodec":"avc1.640028","acodec":"none","protocol":"https","url":"https://example.test/video","format_id":"fixture"})");
        return 0;
    }
    if (args.contains("-o")) {
        std::fputs("fixture media child failed\n", stderr);
        return 1;
    }
    if (args.contains("--eof-test")) {
        StreamConfig config;
        config.sourceKind = SourceKind::Youtube;
        config.youtube.url = QUrl("https://example.test/video");
        config.youtube.formatSelector = "fixture";
        config.youtube.ytDlpPath = QCoreApplication::applicationFilePath();
        YtdlpSource source;
        source.setConfig(config);
        bool reported = false;
        QObject::connect(&source, &StreamSource::errorOccurred, &app, [&](const QString &message) {
            reported = message.contains("fixture media child failed");
            app.quit();
        });
        QTimer::singleShot(5000, &app, &QCoreApplication::quit);
        source.start();
        app.exec();
        source.stop();
        std::puts(reported ? "PASS: empty/failed media pipe reports EOF without spinning"
                           : "FAIL: media EOF was not reported promptly with its cause");
        return reported ? 0 : 1;
    }
    if (argc < 5) {
        std::puts("Usage: youtube-merge-playback-test URL video+audio width height [yt-dlp.exe]");
        return 1;
    }
    StreamConfig config;
    config.sourceKind = SourceKind::Youtube;
    config.youtube.url = QUrl(QString::fromLocal8Bit(argv[1]));
    config.youtube.formatSelector = QString::fromLocal8Bit(argv[2]);
    config.youtube.audioBitrateKbps = 192;
    if (argc > 5) config.youtube.ytDlpPath = QString::fromLocal8Bit(argv[5]);
    const int expectedWidth = QString::fromLocal8Bit(argv[3]).toInt();
    const int expectedHeight = QString::fromLocal8Bit(argv[4]).toInt();
    YtdlpSource source;
    source.setConfig(config);
    BitstreamInspector inspector;
    std::atomic<int> video {0}, audio {0}, width {0}, height {0};
    source.setVideoCallback([&](const std::uint8_t *data, std::size_t size, quint32, quint32) {
        if (video == 0) inspector.init(source.negotiatedVideoCodec());
        const auto info = inspector.inspect(data, size);
        width = info.width;
        height = info.height;
        ++video;
    });
    source.setAudioCallback([&](const std::uint8_t *, std::size_t, quint32) { ++audio; });
    int failures = 0;
    auto check = [&](bool condition, const char *message) {
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
        if (!condition) ++failures;
    };
    QObject::connect(&source, &StreamSource::errorOccurred, &app, [&](const QString &message) {
        std::fprintf(stderr, "Source error: %s\n", qPrintable(message));
        ++failures;
        app.quit();
    });
    QElapsedTimer overall, phaseTime;
    overall.start();
    phaseTime.start();
    int phase = 0, pausedVideo = 0, pausedAudio = 0, resumeVideo = 0;
    double pausePosition = 0;
    QTimer poll;
    int lastReportedSeconds = -1;
    poll.setInterval(100);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        const int seconds = int(overall.elapsed() / 1000);
        if (seconds % 5 == 0 && seconds != lastReportedSeconds) {
            lastReportedSeconds = seconds;
            std::printf("%ds: phase %d, %d video packets, position %.2fs\n", seconds, phase,
                video.load(), source.mediaPositionSeconds());
        }
        if (overall.elapsed() > 60000) {
            check(false, "playback/control test completed within 60 seconds");
            app.quit();
            return;
        }
        if (phase == 0 && video > 60 && source.mediaPositionSeconds() > 2.0) {
            check(width == expectedWidth && height == expectedHeight, "merged output keeps the selected resolution");
            check(audio > 0 && source.negotiatedAudioCodec() == AudioCodec::Opus, "independent audio reaches the Opus pipeline");
            source.requestPause();
            phase = 1;
            phaseTime.restart();
        } else if (phase == 1 && phaseTime.elapsed() > 200) {
            pausedVideo = video;
            pausedAudio = audio;
            pausePosition = source.mediaPositionSeconds();
            phase = 2;
            phaseTime.restart();
        } else if (phase == 2 && phaseTime.elapsed() > 1000) {
            check(video == pausedVideo && audio == pausedAudio, "pause stops both tracks without dropping packets");
            check(source.mediaPositionSeconds() == pausePosition, "pause holds the media clock");
            resumeVideo = video;
            source.requestResume();
            phase = 3;
            phaseTime.restart();
        } else if (phase == 3 && video > resumeVideo + 30 && phaseTime.elapsed() > 1000) {
            check(source.mediaPositionSeconds() > pausePosition + 0.5, "resume advances both playback and media clock");
            source.requestSeek(10000);
            resumeVideo = video;
            phase = 4;
            phaseTime.restart();
        } else if (phase == 4 && source.mediaPositionSeconds() >= 10.5 && video > resumeVideo + 30) {
            check(source.mediaPositionSeconds() < 15.0, "seek restarts the merged tracks at the requested position");
            check(width == expectedWidth && height == expectedHeight, "seek preserves resolution");
            source.requestPause();
            QElapsedTimer shutdown;
            shutdown.start();
            source.stop();
            check(shutdown.elapsed() < 5000, "stop terminates the yt-dlp/FFmpeg process tree while paused");
            std::printf("%d video packets (%dx%d), %d audio packets; %s\n", video.load(), width.load(),
                height.load(), audio.load(), qPrintable(source.lastEvent()));
            phase = 5;
            app.quit();
        }
    });
    poll.start();
    source.start();
    app.exec();
    source.stop();
    return failures > 0 || phase != 5 ? 1 : 0;
}
