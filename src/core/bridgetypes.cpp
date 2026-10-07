/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/bridgetypes.h"

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

QString toString(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264: return u"h264"_s;
    case VideoCodec::H265: return u"h265"_s;
    case VideoCodec::Vp9: return u"vp9"_s;
    case VideoCodec::Av1: return u"av1"_s;
    case VideoCodec::Unknown: break;
    }
    return u"unknown"_s;
}

VideoCodec videoCodecFromString(const QString &text)
{
    const QString normalized = text.trimmed().toLower();
    if (normalized == u"h264"_s || normalized == u"avc"_s) {
        return VideoCodec::H264;
    }
    if (normalized == u"h265"_s || normalized == u"hevc"_s) {
        return VideoCodec::H265;
    }
    if (normalized == u"vp9"_s) {
        return VideoCodec::Vp9;
    }
    if (normalized == u"av1"_s) {
        return VideoCodec::Av1;
    }
    return VideoCodec::Unknown;
}

QString toString(SinkKind kind)
{
    switch (kind) {
    case SinkKind::TsMulticast: return u"ts-multicast"_s;
    case SinkKind::RtpMulticast: return u"rtp-multicast"_s;
    case SinkKind::RtspUnicast: return u"rtsp-unicast"_s;
    case SinkKind::Ndi: return u"ndi"_s;
    }
    return u"unknown"_s;
}

SinkKind sinkKindFromString(const QString &text, bool *ok)
{
    const QString normalized = text.trimmed().toLower();
    if (ok) {
        *ok = true;
    }
    if (normalized == u"ts-multicast"_s || normalized == u"ts"_s) {
        return SinkKind::TsMulticast;
    }
    if (normalized == u"rtp-multicast"_s || normalized == u"rtp"_s) {
        return SinkKind::RtpMulticast;
    }
    if (normalized == u"rtsp-unicast"_s || normalized == u"rtsp"_s) {
        return SinkKind::RtspUnicast;
    }
    if (normalized == u"ndi"_s) {
        return SinkKind::Ndi;
    }
    if (ok) {
        *ok = false;
    }
    return SinkKind::TsMulticast;
}

QString toString(SourceKind kind)
{
    switch (kind) {
    case SourceKind::Whep: return u"whep"_s;
    case SourceKind::Srt: return u"srt"_s;
    case SourceKind::Youtube: return u"youtube"_s;
    }
    return u"unknown"_s;
}

SourceKind sourceKindFromString(const QString &text, bool *ok)
{
    const QString normalized = text.trimmed().toLower();
    if (ok) {
        *ok = true;
    }
    if (normalized == u"whep"_s || normalized == u"webrtc"_s) {
        return SourceKind::Whep;
    }
    if (normalized == u"srt"_s) {
        return SourceKind::Srt;
    }
    if (normalized == u"youtube"_s || normalized == u"yt-dlp"_s) {
        return SourceKind::Youtube;
    }
    if (ok) {
        *ok = false;
    }
    return SourceKind::Whep;
}

QString toString(StreamState state)
{
    switch (state) {
    case StreamState::Idle: return u"Idle"_s;
    case StreamState::Connecting: return u"Connecting"_s;
    case StreamState::Running: return u"Running"_s;
    case StreamState::Paused: return u"Paused"_s;
    case StreamState::Retrying: return u"Retrying"_s;
    case StreamState::Failed: return u"Failed"_s;
    case StreamState::Stopping: return u"Stopping"_s;
    }
    return u"Unknown"_s;
}

QString toString(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::Opus: return u"opus"_s;
    case AudioCodec::Aac: return u"aac"_s;
    case AudioCodec::AacLatm: return u"aac_latm"_s;
    case AudioCodec::Unknown: break;
    }
    return u"unknown"_s;
}

} // namespace CBridge
