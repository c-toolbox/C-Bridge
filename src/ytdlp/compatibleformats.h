/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QVariantList>
#include <algorithm>

namespace CBridge {

inline bool youtubeVideoPassthrough(const QString &codec)
{
    return codec.startsWith("avc1") || codec.startsWith("h264")
        || codec.startsWith("hev1") || codec.startsWith("hvc1") || codec.startsWith("hevc");
}

inline bool usableYoutubeFormat(const QJsonObject &format)
{
    const auto protocol = format.value("protocol").toString();
    return !format.value("format_id").toString().isEmpty()
        && !format.value("url").toString().isEmpty() && !format.value("has_drm").toBool()
        && (protocol == "https" || protocol == "http" || protocol == "http_dash_segments"
            || protocol.startsWith("m3u8"));
}

inline bool supportedYoutubeAudio(const QString &codec)
{
    return codec.startsWith("mp4a") || codec.startsWith("aac") || codec.startsWith("opus")
        || codec.startsWith("vorbis") || codec.startsWith("mp3") || codec.startsWith("flac")
        || codec.startsWith("ac-3") || codec.startsWith("ec-3");
}

inline QString youtubeAudioLabel(const QJsonObject &format)
{
    const auto codec = format.value("acodec").toString();
    QString label = codec.startsWith("mp4a") || codec.startsWith("aac") ? QStringLiteral("AAC") : codec;
    const auto language = format.value("language").toString();
    if (!language.isEmpty()) label += QStringLiteral(" · %1").arg(language);
    const auto note = format.value("format_note").toString();
    if (!note.isEmpty()) label += QStringLiteral(" · %1").arg(note);
    const double abr = format.value("abr").toDouble();
    if (abr > 0) label += QStringLiteral(" · %1 kbps").arg(qRound(abr));
    const int channels = format.value("audio_channels").toInt();
    if (channels > 0) label += QStringLiteral(" · %1 channels").arg(channels);
    return label + QStringLiteral(" (format %1)").arg(format.value("format_id").toString());
}

// Each row is a playable video/audio choice. FFmpeg merges the independent inputs
// to MPEG-TS and converts VP9/AV1 to H.264; the pipeline still sees H.264/H.265 + Opus.
inline QVariantList compatibleYoutubeFormats(const QJsonArray &formats)
{
    QVariantList result;
    QList<QJsonObject> audioFormats;
    for (const auto &value : formats) {
        const auto f = value.toObject();
        if (usableYoutubeFormat(f) && f.value("vcodec").toString() == "none"
            && supportedYoutubeAudio(f.value("acodec").toString())) audioFormats.append(f);
    }
    std::stable_sort(audioFormats.begin(), audioFormats.end(), [](const auto &a, const auto &b) {
        return a.value("abr").toDouble() > b.value("abr").toDouble();
    });
    for (const auto &value : formats) {
        const auto f = value.toObject();
        const auto video = f.value("vcodec").toString();
        const auto audio = f.value("acodec").toString();
        const auto id = f.value("format_id").toString();
        if (!usableYoutubeFormat(f)
            || (!youtubeVideoPassthrough(video) && !video.startsWith("vp9")
                && !video.startsWith("vp09") && !video.startsWith("av01"))
            || (audio != "none" && !supportedYoutubeAudio(audio))) {
            continue;
        }
        const int width = f.value("width").toInt();
        const int height = f.value("height").toInt();
        if (width <= 0 || height <= 0) continue;
        const double fps = f.value("fps").toDouble();
        QString resolution = QStringLiteral("%1 × %2").arg(width).arg(height);
        if (fps > 0) resolution += QStringLiteral(" · %1 fps").arg(fps, 0, 'g', 4);
        QString codecLabel = video.startsWith("avc1") || video.startsWith("h264") ? QStringLiteral("H.264")
            : (youtubeVideoPassthrough(video) ? QStringLiteral("H.265")
                : (video.startsWith("av01") ? QStringLiteral("AV1 → H.264") : QStringLiteral("VP9 → H.264")));
        const auto dynamicRange = f.value("dynamic_range").toString();
        if (!dynamicRange.isEmpty() && dynamicRange != "SDR") {
            if (youtubeVideoPassthrough(video)) codecLabel += QStringLiteral(" → H.264");
            codecLabel += QStringLiteral(" · %1 → SDR").arg(dynamicRange);
        }
        resolution += QStringLiteral(" · %1").arg(codecLabel);
        const QVariantMap base {{"videoId", id}, {"resolution", resolution},
            {"width", width}, {"height", height}, {"fps", fps},
            {"bitrate", f.value("tbr").toDouble()}};
        auto append = [&](const QString &selector, const QString &label, const QString &audioId, bool hasAudio) {
            auto row = base;
            row.insert("id", selector);
            row.insert("audio", label + QStringLiteral(" · video %1").arg(id));
            row.insert("audioId", audioId);
            row.insert("hasAudio", hasAudio);
            result.append(row);
        };
        if (audio != "none") append(id, QStringLiteral("Bundled %1").arg(youtubeAudioLabel(f)), {}, true);
        for (const auto &a : audioFormats) {
            const auto aid = a.value("format_id").toString();
            append(id + "+" + aid, youtubeAudioLabel(a), aid, true);
        }
        if (audio == "none") append(id, QStringLiteral("No audio"), {}, false);
    }
    std::stable_sort(result.begin(), result.end(), [](const QVariant &a, const QVariant &b) {
        const auto x = a.toMap(), y = b.toMap();
        for (const auto &key : {"height", "width", "fps", "bitrate"}) {
            if (x.value(key).toDouble() != y.value(key).toDouble())
                return x.value(key).toDouble() > y.value(key).toDouble();
        }
        return false;
    });
    return result;
}

} // namespace CBridge
