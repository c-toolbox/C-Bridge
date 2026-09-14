#pragma once

#include "config/bridgeconfig.h"
#include "media/audiodecoder.h"
#include "media/videodecoder.h"
#include "media/videofilter.h"
#include "sinks/streamsink.h"

#include <atomic>
#include <vector>

struct NDIlib_send_instance_type;

namespace CBridge {

/// Decodes the incoming bitstream and publishes it as an NDI source.
///
/// Unlike the multicast sink this one cannot pass through: NDI carries its own
/// codec, so the stream has to be decoded (NVDEC where available), converted on
/// the GPU, and handed over as UYVY. The decode chain lives inside the sink so the
/// pipeline stays uniform; a stream with two NDI sinks would decode twice, which is
/// not a configuration worth optimising for.
class NdiSink final : public StreamSink
{
public:
    NdiSink(NdiSinkConfig config, QString streamName);
    ~NdiSink() override;

    QString describe() const override;

    bool open(const StreamFormat &format, QString *error) override;
    void close() override;
    bool isOpen() const override { return m_sender != nullptr; }

    bool writeVideo(const std::uint8_t *data, std::size_t size,
                    std::uint32_t rtpTimestamp, bool isKeyframe) override;
    bool writeAudio(const std::uint8_t *data, std::size_t size,
                    std::uint32_t rtpTimestamp) override;

    quint64 bytesWritten() const override
    {
        return m_bytesWritten.load(std::memory_order_relaxed);
    }

private:
    bool ensureSender(int width, int height, QString *error);
    void onDecodedVideo(AVFrame *frame);
    void onFilteredVideo(AVFrame *frame);
    void onDecodedAudio(AVFrame *frame);

    /// 90 kHz RTP ticks converted to the 100 ns units NDI timecodes use.
    std::int64_t toNdiTimecode(std::int64_t rtpTicks, int clockRate) const;

    NdiSinkConfig m_config;
    QString m_streamName;

    NDIlib_send_instance_type *m_sender = nullptr;
    int m_senderWidth = 0;
    int m_senderHeight = 0;

    VideoDecoder m_videoDecoder;
    VideoFilter m_videoFilter;
    AudioDecoder m_audioDecoder;
    bool m_filterReady = false;

    /// Async sends read from the buffer until the following send returns, so two
    /// buffers alternate and neither is overwritten while in flight.
    std::vector<std::uint8_t> m_frameBuffers[2];
    int m_activeBuffer = 0;

    std::vector<float> m_audioBuffer;

    std::int64_t m_videoLastRaw = -1;
    std::int64_t m_videoOffset = 0;
    std::int64_t m_ptsBase = -1;
    std::int64_t m_currentTimecode = 0;

    std::atomic<quint64> m_bytesWritten { 0 };
};

} // namespace CBridge
