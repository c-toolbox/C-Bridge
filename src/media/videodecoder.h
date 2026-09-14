#pragma once

#include "core/bridgetypes.h"
#include "media/avwrappers.h"

#include <QString>

#include <cstdint>
#include <functional>

namespace CBridge {

/// Decodes Annex-B access units, preferring NVDEC.
///
/// There is no AVFormatContext: access units arrive already framed from the RTP
/// depacketizer, so they go straight into avcodec_send_packet.
class VideoDecoder
{
public:
    /// Invoked for each decoded frame. The frame is owned by the decoder and is
    /// only valid for the duration of the call.
    using FrameCallback = std::function<void(AVFrame *frame)>;

    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    /// Set before open() to force software decoding.
    void setHardwareEnabled(bool enabled) { m_hardwareEnabled = enabled; }

    bool open(VideoCodec codec, QString *error);
    void close();
    bool isOpen() const { return m_context != nullptr; }

    bool isHardware() const { return m_hardware; }
    int width() const;
    int height() const;

    void setFrameCallback(FrameCallback callback) { m_onFrame = std::move(callback); }

    bool decode(const std::uint8_t *data, std::size_t size, std::int64_t pts, QString *error);
    void flush();

private:
    bool drain(QString *error);

    CodecContextPtr m_context;
    FramePtr m_frame;
    PacketPtr m_packet;

    FrameCallback m_onFrame;

    bool m_hardwareEnabled = true;
    bool m_hardware = false;
};

} // namespace CBridge
