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

} // namespace

std::vector<std::uint8_t> BitstreamInspector::extractParameterSets(VideoCodec codec,
                                                                   const std::uint8_t *data,
                                                                   std::size_t size)
{
    std::vector<std::uint8_t> sets;
    if (!data || size < 4) {
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
    m_width = 0;
    m_height = 0;
}

bool BitstreamInspector::init(VideoCodec codec)
{
    reset();
    m_codec = codec;

    const AVCodecID codecId = (codec == VideoCodec::H265) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;

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

    const NalScan scan = scanNalUnits(m_codec, data, size);
    result.isKeyframe = scan.keyframe;
    result.hasParameterSets = scan.parameterSets;
    m_hasParameterSets = m_hasParameterSets || scan.parameterSets;

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
