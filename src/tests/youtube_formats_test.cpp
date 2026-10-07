/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "models/streamdraft.h"
#include "ytdlp/compatibleformats.h"
#include "ytdlp/ffmpegstreamoptions.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QThread>
#include <cstdio>

using namespace CBridge;

static const QByteArray metadata = R"({"formats":[
 {"format_id":"720-en","protocol":"m3u8_native","vcodec":"avc1.64001f","acodec":"mp4a.40.2","width":1280,"height":720,"fps":30,"abr":128,"language":"en","audio_channels":2,"url":"https://example.test/a"},
 {"format_id":"1080-en","protocol":"m3u8_native","vcodec":"avc1.640028","acodec":"mp4a.40.2","width":1920,"height":1080,"fps":60,"abr":128,"language":"en","url":"https://example.test/b"},
 {"format_id":"720-sv","protocol":"m3u8_native","vcodec":"avc1.64001f","acodec":"mp4a.40.2","width":1280,"height":720,"fps":30,"abr":96,"language":"sv","url":"https://example.test/c"},
 {"format_id":"vp9","protocol":"m3u8_native","vcodec":"vp9","acodec":"opus","width":3840,"height":2160,"url":"https://example.test/d"},
 {"format_id":"dash","protocol":"https","vcodec":"avc1.640028","acodec":"none","width":1920,"height":1080,"fps":60,"url":"https://example.test/e"},
 {"format_id":"drm","protocol":"m3u8_native","vcodec":"avc1.640028","acodec":"mp4a.40.2","width":1920,"height":1080,"has_drm":true,"url":"https://example.test/f"},
 {"format_id":"unsupported-audio","protocol":"m3u8_native","vcodec":"avc1.640028","acodec":"dts","width":1920,"height":1080,"url":"https://example.test/g"},
 {"format_id":"silent","protocol":"m3u8_native","vcodec":"avc1.64001e","acodec":"none","width":640,"height":360,"url":"https://example.test/h"},
 {"format_id":"av1","protocol":"https","vcodec":"av01.0.13M.10","acodec":"none","width":3840,"height":2160,"fps":60,"dynamic_range":"HDR10","url":"https://example.test/i"},
 {"format_id":"hevc","protocol":"https","vcodec":"hvc1.1.6.L150","acodec":"none","width":2560,"height":1440,"fps":60,"url":"https://example.test/j"},
 {"format_id":"audio-en","protocol":"https","vcodec":"none","acodec":"mp4a.40.2","abr":128,"language":"en","url":"https://example.test/k"},
 {"format_id":"audio-sv","protocol":"https","vcodec":"none","acodec":"mp4a.40.2","abr":96,"language":"sv","url":"https://example.test/l"},
 {"format_id":"audio-opus","protocol":"https","vcodec":"none","acodec":"opus","abr":160,"language":"en","audio_channels":2,"url":"https://example.test/m"},
 {"format_id":"drm-audio","protocol":"https","vcodec":"none","acodec":"opus","abr":200,"has_drm":true,"url":"https://example.test/n"}
]})";

static bool waitForProbe(StreamDraft &draft, int timeout = 6000)
{
    QElapsedTimer timer;
    timer.start();
    while (draft.youtubeFormatsBusy() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    return !draft.youtubeFormatsBusy();
}

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    // Act as a deterministic yt-dlp child for asynchronous probing tests.
    if (args.contains("--dump-single-json")) {
        const auto url = args.last();
        if (url.contains("slow")) QThread::msleep(1200);
        if (url.contains("fail")) {
            std::fprintf(stderr, "fixture authentication failure");
            return 1;
        }
        const auto output = url.contains("bad") ? QByteArray("not json") : metadata;
        std::fwrite(output.constData(), 1, size_t(output.size()), stdout);
        return 0;
    }
    const auto formats = compatibleYoutubeFormats(QJsonDocument::fromJson(metadata).object().value("formats").toArray());
    CHECK(formats.size() == 32);
    CHECK(formats.first().toMap().value("id") == "av1+audio-opus");
    CHECK(!formats.last().toMap().value("hasAudio").toBool());
    CHECK(youtubeNeedsVideoConversion("vp9", "SDR"));
    CHECK(youtubeNeedsVideoConversion("av01.0.13M.10", "HDR10"));
    CHECK(!youtubeNeedsVideoConversion("hvc1.1.6.L150", "SDR"));
    CHECK(!youtubeNeedsVideoConversion("vp9", "SDR", true));
    CHECK(!youtubeNeedsVideoConversion("av01.0.08M.08", "SDR", true));
    CHECK(youtubeNeedsVideoConversion("av01.0.13M.10", "HDR10", true));
    const auto nativeOptions = youtubeFfmpegOutputOptions(false, false, false, 128, true, true);
    CHECK(nativeOptions.contains("-f matroska -live 1"));
    CHECK(!nativeOptions.contains("mpegts"));
    CHECK(!nativeOptions.contains("-c:v"));
    const auto copyOptions = youtubeFfmpegOutputOptions(false, false, false, 192, true);
    CHECK(copyOptions.contains("-f mpegts"));
    CHECK(copyOptions.contains("-b:a 192k"));
    CHECK(copyOptions.contains("-map -0:a?"));
    CHECK(!copyOptions.contains("-c:v"));
    const auto convertOptions = youtubeFfmpegOutputOptions(true, true, true, 128, true);
    CHECK(convertOptions.contains("h264_nvenc"));
    CHECK(convertOptions.contains("tonemap=hable"));
    CHECK(!convertOptions.contains("-s ")); // Keep the selected resolution.

    StreamConfig config;
    config.sourceKind = SourceKind::Youtube;
    config.youtube.url = QUrl("https://example.test/video");
    config.youtube.ytDlpPath = QCoreApplication::applicationFilePath();
    config.youtube.formatSelector = "720-sv";
    StreamDraft draft;
    auto resolutionIndex = [&draft](const QString &part) {
        for (int i = 0; i < draft.youtubeResolutions().size(); ++i)
            if (draft.youtubeResolutions().at(i).contains(part)) return i;
        return -1;
    };
    auto audioIndex = [&draft](const QString &selector) {
        for (int i = 0; i < draft.youtubeAudioOptions().size(); ++i)
            if (draft.youtubeAudioOptions().at(i).toMap().value("id").toString() == selector) return i;
        return -1;
    };
    draft.load(config);
    CHECK(draft.youtubeFormatsBusy());
    CHECK(waitForProbe(draft));
    CHECK(draft.youtubeFormatsError().isEmpty());
    CHECK(draft.youtubeResolutions().size() == 6);
    const int r720 = resolutionIndex("1280");
    const int r1080 = resolutionIndex("1920");
    const int r360 = resolutionIndex("640");
    CHECK(draft.youtubeResolutionIndex() == r720);
    CHECK(draft.youtubeAudioOptions().size() == 8);
    CHECK(draft.youtubeAudioIndex() == audioIndex("720-sv"));
    draft.selectYoutubeResolution(r1080);
    CHECK(draft.formatSelector() == "1080-en");
    CHECK(audioIndex("dash+audio-opus") >= 0);
    draft.selectYoutubeAudio(audioIndex("dash+audio-opus"));
    CHECK(draft.formatSelector() == "dash+audio-opus");
    draft.selectYoutubeResolution(r720);
    CHECK(draft.formatSelector() == "720-en+audio-opus"); // Audio stays independent of video.
    draft.selectYoutubeAudio(audioIndex("720-en+audio-sv"));
    CHECK(draft.formatSelector() == "720-en+audio-sv");
    CHECK(draft.youtubeAudioOptions().at(draft.youtubeAudioIndex()).toMap().value("audio").toString().contains("sv"));
    const auto saved = StreamConfig::fromJson(draft.toConfig().toJson());
    CHECK(saved.youtube.formatSelector == "720-en+audio-sv");
    draft.load(saved);
    CHECK(waitForProbe(draft));
    CHECK(draft.youtubeAudioIndex() == audioIndex("720-en+audio-sv"));
    draft.selectYoutubeResolution(r360);
    draft.selectYoutubeAudio(audioIndex("silent"));
    CHECK(draft.formatSelector() == "silent");
    CHECK(!draft.isAudioEnabled());
    draft.selectYoutubeResolution(r1080);
    CHECK(draft.isAudioEnabled());
    draft.selectYoutubeResolution(resolutionIndex("AV1"));
    draft.selectYoutubeAudio(audioIndex("av1+audio-opus"));
    CHECK(draft.formatSelector() == "av1+audio-opus");
    draft.selectYoutubeResolution(resolutionIndex("H.265"));
    CHECK(draft.formatSelector() == "hevc+audio-opus");

    draft.setYoutubeUrl("https://example.test/slow");
    draft.refreshYoutubeFormats();
    QCoreApplication::processEvents();
    draft.setYoutubeUrl("https://example.test/fail");
    CHECK(draft.youtubeResolutions().isEmpty());
    CHECK(waitForProbe(draft));
    CHECK(draft.youtubeFormatsError().contains("fixture authentication failure"));
    CHECK(draft.youtubeResolutions().isEmpty());
    draft.setYoutubeUrl("https://example.test/bad");
    CHECK(waitForProbe(draft));
    CHECK(draft.youtubeFormatsError().contains("invalid"));
    draft.setYoutubeUrl("https://example.test/retry");
    CHECK(waitForProbe(draft));
    CHECK(draft.youtubeFormatsError().isEmpty());
    CHECK(draft.youtubeResolutions().size() == 6);
    CHECK(draft.youtubeResolutionIndex() == -1); // New URL clears a previous format ID.
    draft.setSourceKind("whep");
    CHECK(!draft.youtubeFormatsBusy());
    CHECK(draft.youtubeResolutions().isEmpty());

    if (args.size() > 1) {
        QFile file(args.at(1));
        CHECK(file.open(QIODevice::ReadOnly));
        const auto real = compatibleYoutubeFormats(QJsonDocument::fromJson(file.readAll()).object().value("formats").toArray());
        CHECK(!real.isEmpty());
        std::printf("Real YouTube metadata: %lld compatible formats\n", static_cast<long long>(real.size()));
        QStringList labels;
        for (const auto &value : real) {
            const auto f = value.toMap();
            const auto label = f.value("resolution").toString();
            if (!labels.contains(label)) {
                labels.append(label);
                if (labels.size() <= 6) std::printf("  %s\n", qPrintable(label));
            }
        }
        std::printf("  %lld resolution/codec choices\n", static_cast<long long>(labels.size()));
    }
    std::puts("PASS: independent video/audio pairing, codec conversion, persistence, cancellation, failures and retry");
    return 0;
}
