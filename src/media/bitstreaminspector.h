/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "core/bridgetypes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct AVCodecContext;
struct AVCodecParserContext;

namespace CBridge {

/// Pulls the few facts the muxer needs out of an Annex-B access unit without
/// decoding it: whether it is a keyframe, and the coded resolution.
///
/// Keyframe detection is a direct NAL-type scan because it must be reliable from
/// the very first access unit, before the parser has locked on. Resolution comes
/// from FFmpeg's parser, which already knows how to read an SPS.
class BitstreamInspector
{
public:
    struct Result {
        bool isKeyframe = false;
        /// True when this access unit itself carries SPS/PPS (and VPS for HEVC).
        bool hasParameterSets = false;
        int width = 0;
        int height = 0;
    };

    BitstreamInspector();
    ~BitstreamInspector();

    BitstreamInspector(const BitstreamInspector &) = delete;
    BitstreamInspector &operator=(const BitstreamInspector &) = delete;

    bool init(VideoCodec codec);
    void reset();

    VideoCodec codec() const { return m_codec; }

    Result inspect(const std::uint8_t *data, std::size_t size);

    /// Returns just the SPS/PPS (and VPS for HEVC) NAL units, start codes included.
    static std::vector<std::uint8_t> extractParameterSets(VideoCodec codec,
                                                          const std::uint8_t *data,
                                                          std::size_t size);

    /// True when an SPS (and for HEVC a VPS) has been seen, meaning a decoder or
    /// muxer downstream has enough to start.
    bool hasParameterSets() const { return m_hasParameterSets; }

private:
    VideoCodec m_codec = VideoCodec::Unknown;
    AVCodecParserContext *m_parser = nullptr;
    AVCodecContext *m_codecContext = nullptr;

    bool m_hasParameterSets = false;
    int m_width = 0;
    int m_height = 0;
};

} // namespace CBridge
