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

/// Walks Annex-B start codes and classifies the NAL unit types present.
NalScan scanNalUnits(VideoCodec codec, const std::uint8_t *data, std::size_t size)
{
    NalScan scan;
    if (size < 4) {
        return scan;
    }

    const std::uint8_t *bytes = data;

    for (std::size_t i = 0; i + 3 < size; ++i) {
        if (bytes[i] != 0x00 || bytes[i + 1] != 0x00) {
            continue;
        }

        std::size_t headerStart = 0;
        if (bytes[i + 2] == 0x01) {
            headerStart = i + 3;
        } else if (bytes[i + 2] == 0x00 && i + 4 < size && bytes[i + 3] == 0x01) {
            headerStart = i + 4;
        } else {
            continue;
        }

        if (headerStart >= size) {
            break;
        }

        if (codec == VideoCodec::H264) {
            const unsigned int type = bytes[headerStart] & 0x1F;
            if (type == 5) { // IDR slice
                scan.keyframe = true;
            } else if (type == 7 || type == 8) { // SPS, PPS
                scan.parameterSets = true;
            }
        } else if (codec == VideoCodec::H265) {
            const unsigned int type = (bytes[headerStart] >> 1) & 0x3F;
            if (type >= 16 && type <= 23) { // BLA..CRA, the IRAP range
                scan.keyframe = true;
            } else if (type == 32 || type == 33 || type == 34) { // VPS, SPS, PPS
                scan.parameterSets = true;
            }
        }

        i = headerStart;
    }

    return scan;
}

} // namespace

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
