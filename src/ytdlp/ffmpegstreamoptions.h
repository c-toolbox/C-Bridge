/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "ytdlp/compatibleformats.h"

namespace CBridge {

inline bool youtubeNativeVideo(const QString &codec)
{
    return codec.startsWith(QStringLiteral("vp9")) || codec.startsWith(QStringLiteral("vp09"))
        || codec.startsWith(QStringLiteral("av01"));
}

// decodingSinksOnly: every sink decodes the video itself (NDI), so VP9/AV1 can pass through.
// HDR still converts: the tone mapping to SDR happens in FFmpeg's filter graph.
inline bool youtubeNeedsVideoConversion(const QString &codec, const QString &dynamicRange,
                                        bool decodingSinksOnly = false)
{
    if (!dynamicRange.isEmpty() && dynamicRange != QStringLiteral("SDR")) return true;
    return !youtubeVideoPassthrough(codec) && !(decodingSinksOnly && youtubeNativeVideo(codec));
}

// These options go after yt-dlp's -c copy, mapping and container options.
// MPEG-TS is seek-free and emits Annex-B H.264/HEVC plus self-contained Opus. VP9/AV1 have
// no MPEG-TS mapping in this FFmpeg, so their passthrough uses live (cue-less) Matroska.
inline QString youtubeFfmpegOutputOptions(bool transcodeVideo, bool hdr, bool nvenc,
                                         int audioBitrateKbps, bool separateInputs,
                                         bool matroska = false)
{
    QString options = QStringLiteral("-loglevel error -f %1 -c:a libopus -ar 48000 -ac 2 -b:a %2k -shortest")
        .arg(matroska ? QStringLiteral("matroska -live 1") : QStringLiteral("mpegts"))
        .arg(qBound(32, audioBitrateKbps, 510));
    if (separateInputs) options += QStringLiteral(" -map -0:a?"); // Drop a video's bundled audio, if any.
    if (transcodeVideo) {
        options += nvenc ? QStringLiteral(" -c:v h264_nvenc -preset p4 -cq 20 -b:v 0")
                         : QStringLiteral(" -c:v libx264 -preset ultrafast -crf 20");
        options += QStringLiteral(" -pix_fmt yuv420p -force_key_frames expr:gte(t,n_forced*2)");
        if (hdr) {
            options += QStringLiteral(" -vf zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=hable,zscale=t=bt709:m=bt709:r=tv,format=yuv420p"
                                      " -color_primaries bt709 -color_trc bt709 -colorspace bt709");
        }
    }
    if (!matroska) options += QStringLiteral(" -mpegts_flags +resend_headers");
    return options + QStringLiteral(" -flush_packets 1");
}

} // namespace CBridge
