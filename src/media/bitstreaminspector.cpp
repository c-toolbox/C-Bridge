/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/bitstreaminspector.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace CBridge {

namespace {

struct NalScan {
    bool keyframe = false;
    bool parameterSets = false;
};

/// Calls visit(startCodeOffset, payloadOffset) for every Annex-B NAL unit found.
template<typename Visitor>
void forEachNalUnit(const std::uint8_t *bytes, std::size_t size, Visitor visit)
{
    for (std::size_t i = 0; i + 3 < size; ++i) {
        if (bytes[i] != 0x00 || bytes[i + 1] != 0x00) {
            continue;
        }

        std::size_t payloadStart = 0;
        if (bytes[i + 2] == 0x01) {
            payloadStart = i + 3;
        } else if (bytes[i + 2] == 0x00 && i + 3 < size && bytes[i + 3] == 0x01) {
            payloadStart = i + 4;
        } else {
            continue;
        }

        if (payloadStart >= size) {
            break;
        }

        visit(i, payloadStart);
        i = payloadStart;
    }
}

bool isParameterSetNal(VideoCodec codec, std::uint8_t header)
{
    if (codec == VideoCodec::H265) {
        const unsigned int type = (header >> 1) & 0x3F;
        return type == 32 || type == 33 || type == 34; // VPS, SPS, PPS
    }
    const unsigned int type = header & 0x1F;
    return type == 7 || type == 8; // SPS, PPS
}

/// Walks Annex-B start codes and classifies the NAL unit types present.
NalScan scanNalUnits(VideoCodec codec, const std::uint8_t *data, std::size_t size)
{
    NalScan scan;
    if (size < 4) {
        return scan;
    }

    forEachNalUnit(data, size, [&](std::size_t, std::size_t payloadStart) {
        const std::uint8_t header = data[payloadStart];
        if (isParameterSetNal(codec, header)) {
            scan.parameterSets = true;
            return;
        }
        if (codec == VideoCodec::H265) {
            const unsigned int type = (header >> 1) & 0x3F;
            if (type >= 16 && type <= 23) { // BLA..CRA, the IRAP range
                scan.keyframe = true;
            }
        } else if ((header & 0x1F) == 5) { // IDR slice
            scan.keyframe = true;
        }
    });

    return scan;
}

/// MSB-first reader; reads past the end yield zeros and clear ok().
class BitReader
{
public:
    BitReader(const std::uint8_t *data, std::size_t size)
        : m_data(data)
        , m_bits(size * 8)
    {
    }

    unsigned read(int count)
    {
        unsigned value = 0;
        for (int i = 0; i < count; ++i) {
            unsigned bit = 0;
            if (m_pos < m_bits) {
                bit = (m_data[m_pos / 8] >> (7 - m_pos % 8)) & 1u;
            } else {
                m_ok = false;
            }
            ++m_pos;
            value = (value << 1) | bit;
        }
        return value;
    }

    bool ok() const { return m_ok; }

private:
    const std::uint8_t *m_data;
    std::size_t m_bits;
    std::size_t m_pos = 0;
    bool m_ok = true;
};

struct FrameScan {
    bool keyframe = false;
    int width = 0;
    int height = 0;
};

/// Reads the VP9 uncompressed header (spec 6.2) far enough for the frame type and size.
/// A superframe starts with its first frame, so the leading bytes are a frame header too.
FrameScan scanVp9Frame(const std::uint8_t *data, std::size_t size)
{
    FrameScan scan;
    BitReader reader(data, size);
    if (reader.read(2) != 2) { // frame_marker
        return scan;
    }
    const unsigned profileLow = reader.read(1);
    const unsigned profile = (reader.read(1) << 1) | profileLow;
    if (profile == 3) {
        reader.read(1); // reserved_zero
    }
    if (reader.read(1) || reader.read(1)) { // show_existing_frame, frame_type (0 = key)
        return scan;
    }
    scan.keyframe = true;

    reader.read(2); // show_frame, error_resilient_mode
    if (reader.read(24) != 0x498342) { // frame_sync_code
        return scan;
    }
    if (profile >= 2) {
        reader.read(1); // ten_or_twelve_bit
    }
    const bool subsamplingBits = profile == 1 || profile == 3;
    if (reader.read(3) != 7) { // color_space != CS_RGB
        reader.read(subsamplingBits ? 4 : 1); // color_range [, subsampling_x/y, reserved]
    } else if (subsamplingBits) {
        reader.read(1); // reserved_zero
    }
    const int width = int(reader.read(16)) + 1;
    const int height = int(reader.read(16)) + 1;
    if (reader.ok()) {
        scan.width = width;
        scan.height = height;
    }
    return scan;
}

bool readLeb128(const std::uint8_t *data, std::size_t size, std::size_t &pos, std::uint64_t &value)
{
    value = 0;
    for (int i = 0; i < 8; ++i) {
        if (pos >= size) {
            return false;
        }
        const std::uint8_t byte = data[pos++];
        value |= std::uint64_t(byte & 0x7F) << (7 * i);
        if (!(byte & 0x80)) {
            return true;
        }
    }
    return false;
}

/// Walks the low-overhead OBUs of one AV1 temporal unit (spec 5.3) to find whether its
/// first frame is a key frame. A sequence header updates reducedStillPicture on the way.
bool scanAv1KeyFrame(const std::uint8_t *data, std::size_t size, bool &reducedStillPicture)
{
    constexpr unsigned kObuSequenceHeader = 1;
    constexpr unsigned kObuFrameHeader = 3;
    constexpr unsigned kObuFrame = 6;

    std::size_t pos = 0;
    while (pos < size) {
        const std::uint8_t header = data[pos];
        const unsigned type = (header >> 3) & 0x0F;
        const bool extension = (header >> 2) & 1;
        const bool hasSize = (header >> 1) & 1;
        pos += extension ? 2 : 1;

        std::uint64_t obuSize = 0;
        if (hasSize) {
            if (!readLeb128(data, size, pos, obuSize)) {
                return false;
            }
        } else {
            obuSize = pos < size ? size - pos : 0;
        }
        if (pos >= size || obuSize > size - pos) {
            return false;
        }

        const std::uint8_t *payload = data + pos;
        if (type == kObuSequenceHeader) {
            BitReader reader(payload, std::size_t(obuSize));
            reader.read(4); // seq_profile, still_picture
            reducedStillPicture = reader.read(1) != 0;
        } else if (type == kObuFrameHeader || type == kObuFrame) {
            if (reducedStillPicture) {
                return true;
            }
            const std::uint8_t first = payload[0];
            // show_existing_frame = 0 and frame_type = KEY_FRAME (0).
            return obuSize > 0 && !(first & 0x80) && ((first >> 5) & 0x03) == 0;
        }
        pos += std::size_t(obuSize);
    }
    return false;
}

} // namespace

std::vector<std::uint8_t> BitstreamInspector::extractParameterSets(VideoCodec codec,
                                                                   const std::uint8_t *data,
                                                                   std::size_t size)
{
    std::vector<std::uint8_t> sets;
    if (!data || size < 4 || !usesParameterSets(codec)) {
        return sets;
    }

    std::vector<std::pair<std::size_t, std::size_t>> starts; // start code offset, payload offset
    forEachNalUnit(data, size, [&](std::size_t startCode, std::size_t payloadStart) {
        starts.emplace_back(startCode, payloadStart);
    });

    for (std::size_t n = 0; n < starts.size(); ++n) {
        if (!isParameterSetNal(codec, data[starts[n].second])) {
            continue;
        }
        const std::size_t begin = starts[n].first;
        const std::size_t end = (n + 1 < starts.size()) ? starts[n + 1].first : size;
        sets.insert(sets.end(), data + begin, data + end);
    }

    return sets;
}

BitstreamInspector::BitstreamInspector() = default;

BitstreamInspector::~BitstreamInspector()
{
    reset();
}

void BitstreamInspector::reset()
{
    if (m_parser) {
        av_parser_close(m_parser);
        m_parser = nullptr;
    }
    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
    }
    m_hasParameterSets = false;
    m_av1ReducedStillPicture = false;
    m_width = 0;
    m_height = 0;
}

bool BitstreamInspector::init(VideoCodec codec)
{
    reset();
    m_codec = codec;

    AVCodecID codecId = AV_CODEC_ID_H264;
    switch (codec) {
    case VideoCodec::H265: codecId = AV_CODEC_ID_HEVC; break;
    case VideoCodec::Vp9: codecId = AV_CODEC_ID_VP9; break;
    case VideoCodec::Av1: codecId = AV_CODEC_ID_AV1; break;
    default: break;
    }

    const AVCodec *decoder = avcodec_find_decoder(codecId);
    if (!decoder) {
        return false;
    }

    m_codecContext = avcodec_alloc_context3(decoder);
    if (!m_codecContext) {
        return false;
    }

    // Never opened, so the parser cannot retain a PPS and would log about it per frame.
    m_codecContext->log_level_offset = AV_LOG_QUIET;

    // Never opened: the parser only reads headers, it does not decode.
    m_parser = av_parser_init(codecId);
    return m_parser != nullptr;
}

BitstreamInspector::Result BitstreamInspector::inspect(const std::uint8_t *data, std::size_t size)
{
    Result result;
    if (!m_parser || !m_codecContext || size == 0) {
        return result;
    }

    if (m_codec == VideoCodec::Vp9) {
        // FFmpeg's VP9 parser reports no dimensions, so the keyframe header supplies them.
        const FrameScan scan = scanVp9Frame(data, size);
        result.isKeyframe = scan.keyframe;
        if (scan.width > 0 && scan.height > 0) {
            m_width = scan.width;
            m_height = scan.height;
        }
        result.width = m_width;
        result.height = m_height;
        return result;
    }

    if (m_codec == VideoCodec::Av1) {
        result.isKeyframe = scanAv1KeyFrame(data, size, m_av1ReducedStillPicture);
    } else {
        const NalScan scan = scanNalUnits(m_codec, data, size);
        result.isKeyframe = scan.keyframe;
        result.hasParameterSets = scan.parameterSets;
        m_hasParameterSets = m_hasParameterSets || scan.parameterSets;
    }

    uint8_t *outBuffer = nullptr;
    int outSize = 0;
    av_parser_parse2(m_parser, m_codecContext, &outBuffer, &outSize,
                     data, int(size),
                     AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);

    if (m_parser->width > 0 && m_parser->height > 0) {
        m_width = m_parser->width;
        m_height = m_parser->height;
    } else if (m_codecContext->width > 0 && m_codecContext->height > 0) {
        m_width = m_codecContext->width;
        m_height = m_codecContext->height;
    }

    result.width = m_width;
    result.height = m_height;
    return result;
}

} // namespace CBridge
