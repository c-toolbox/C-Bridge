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
    return VideoCodec::Unknown;
}

QString toString(SinkKind kind)
{
    switch (kind) {
    case SinkKind::TsMulticast: return u"ts-multicast"_s;
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
    if (normalized == u"ndi"_s) {
        return SinkKind::Ndi;
    }
    if (ok) {
        *ok = false;
    }
    return SinkKind::TsMulticast;
}

QString toString(StreamState state)
{
    switch (state) {
    case StreamState::Idle: return u"Idle"_s;
    case StreamState::Connecting: return u"Connecting"_s;
    case StreamState::Running: return u"Running"_s;
    case StreamState::Retrying: return u"Retrying"_s;
    case StreamState::Failed: return u"Failed"_s;
    case StreamState::Stopping: return u"Stopping"_s;
    }
    return u"Unknown"_s;
}

} // namespace CBridge
