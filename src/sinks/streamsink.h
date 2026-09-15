/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "core/bridgetypes.h"

#include <QString>

#include <cstddef>
#include <cstdint>

namespace CBridge {

/// Everything a sink needs to know before it can accept media.
struct StreamFormat {
    VideoCodec videoCodec = VideoCodec::Unknown;
    int width = 0;
    int height = 0;

    /// Codec extradata (SPS/PPS, plus VPS for HEVC) in Annex-B form. May be empty
    /// when the parameter sets are carried in-band on every keyframe.
    QByteArray videoExtradata;

    bool hasAudio = false;
    int audioSampleRate = 48000;
    int audioChannels = 2;
};

/// One output of a stream pipeline.
///
/// Implementations are driven by a single thread and must never block the caller
/// for long; the pipeline feeds them from a bounded, drop-oldest queue.
class StreamSink
{
public:
    virtual ~StreamSink() = default;

    virtual QString describe() const = 0;

    /// Prepares the sink. Called once the source format is known, which for video
    /// means after the first keyframe has been parsed.
    virtual bool open(const StreamFormat &format, QString *error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    /// rtpTimestamp is the 90 kHz RTP clock for video and the 48 kHz clock for audio.
    virtual bool writeVideo(const std::uint8_t *data, std::size_t size,
                            std::uint32_t rtpTimestamp, bool isKeyframe) = 0;
    virtual bool writeAudio(const std::uint8_t *data, std::size_t size,
                            std::uint32_t rtpTimestamp) = 0;

    /// True when the sink cannot start until it sees a keyframe. Muxers need the
    /// parameter sets before they can write a header.
    virtual bool requiresKeyframeToStart() const { return true; }

    virtual quint64 bytesWritten() const = 0;
};

} // namespace CBridge
