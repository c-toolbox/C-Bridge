#include "media/videodecoder.h"

#include "media/cudacontext.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

/// Pinned by open() so get_format can pick the hardware surface format.
AVPixelFormat g_hwPixelFormat = AV_PIX_FMT_NONE;

AVPixelFormat selectHwFormat(AVCodecContext *, const AVPixelFormat *formats)
{
    for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == g_hwPixelFormat) {
            return *format;
        }
    }
    // Falling back to the first software format keeps decoding alive at CPU cost.
    return formats[0];
}

AVCodecID toAvCodecId(VideoCodec codec)
{
    return codec == VideoCodec::H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
}

} // namespace

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder()
{
    close();
}

int VideoDecoder::width() const
{
    return m_context ? m_context->width : 0;
}

int VideoDecoder::height() const
{
    return m_context ? m_context->height : 0;
}

bool VideoDecoder::open(VideoCodec codec, QString *error)
{
    close();

    if (codec == VideoCodec::Unknown) {
        if (error) {
            *error = u"Cannot open a decoder for an unknown codec"_s;
        }
        return false;
    }

    const AVCodecID codecId = toAvCodecId(codec);
    const AVCodec *decoder = avcodec_find_decoder(codecId);
    if (!decoder) {
        if (error) {
            *error = u"No decoder available for %1"_s.arg(CBridge::toString(codec));
        }
        return false;
    }

    m_context.reset(avcodec_alloc_context3(decoder));
    if (!m_context) {
        if (error) {
            *error = u"Out of memory allocating the decoder context"_s;
        }
        return false;
    }

    m_hardware = false;
    if (m_hardwareEnabled && CudaContext::instance().ensureInitialized()) {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
            if (!config) {
                break;
            }
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                && config->device_type == AV_HWDEVICE_TYPE_CUDA) {
                g_hwPixelFormat = config->pix_fmt;
                m_context->get_format = selectHwFormat;
                m_context->hw_device_ctx =
                    av_buffer_ref(CudaContext::instance().deviceContext());
                m_hardware = m_context->hw_device_ctx != nullptr;
                break;
            }
        }
    }

    // Live streaming: never trade latency for throughput.
    m_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_context->flags2 |= AV_CODEC_FLAG2_FAST;
    m_context->thread_count = m_hardware ? 1 : 2;
    m_context->thread_type = FF_THREAD_SLICE;

    AVCodecContext *raw = m_context.get();
    const int ret = avcodec_open2(raw, decoder, nullptr);
    if (ret < 0) {
        if (error) {
            *error = u"Could not open the %1 decoder: %2"_s
                         .arg(CBridge::toString(codec), avErrorString(ret));
        }
        m_context.reset();
        return false;
    }

    m_frame = makeFrame();
    m_packet = makePacket();
    if (!m_frame || !m_packet) {
        if (error) {
            *error = u"Out of memory allocating decoder buffers"_s;
        }
        close();
        return false;
    }

    qInfo("Opened %s decoder (%s)", qUtf8Printable(CBridge::toString(codec)),
          m_hardware ? "NVDEC" : "software");
    return true;
}

void VideoDecoder::close()
{
    m_frame.reset();
    m_packet.reset();
    m_context.reset();
    m_hardware = false;
}

bool VideoDecoder::decode(const std::uint8_t *data, std::size_t size, std::int64_t pts,
                          QString *error)
{
    if (!m_context || size == 0) {
        return false;
    }

    av_packet_unref(m_packet.get());
    if (av_new_packet(m_packet.get(), int(size)) < 0) {
        if (error) {
            *error = u"Out of memory allocating a decode packet"_s;
        }
        return false;
    }
    std::memcpy(m_packet->data, data, size);
    m_packet->pts = pts;
    m_packet->dts = pts;

    const int ret = avcodec_send_packet(m_context.get(), m_packet.get());
    av_packet_unref(m_packet.get());

    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        if (error) {
            *error = u"Decoder rejected a packet: %1"_s.arg(avErrorString(ret));
        }
        return false;
    }

    return drain(error);
}

bool VideoDecoder::drain(QString *error)
{
    while (true) {
        const int ret = avcodec_receive_frame(m_context.get(), m_frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            if (error) {
                *error = u"Decode failed: %1"_s.arg(avErrorString(ret));
            }
            return false;
        }

        if (m_onFrame) {
            m_onFrame(m_frame.get());
        }
        av_frame_unref(m_frame.get());
    }
}

void VideoDecoder::flush()
{
    if (m_context) {
        avcodec_flush_buffers(m_context.get());
    }
}

} // namespace CBridge
