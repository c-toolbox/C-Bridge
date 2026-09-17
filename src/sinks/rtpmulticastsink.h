/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "sinks/streamsink.h"

#include <atomic>

struct AVFormatContext;
struct AVPacket;

namespace CBridge {

/// Sends the incoming elementary streams as raw RTP over UDP multicast: one FFmpeg
/// rtp muxer context for the H.264/H.265 video (to `port`) and one for the Opus audio
/// (to `port + 1`). Like the TS path this is a pure passthrough - no decode or encode.
///
/// The pipeline emits Annex-B access units with SPS/PPS in-band on keyframes, which is
/// exactly what FFmpeg's H.264/HEVC RTP packetizer expects when no AVCC length size is
/// active: it scans each unit for start codes and STAP-A/FU-fragments the NALs. RTCP
/// sender reports are emitted by the muxer on the same sockets, roughly every 5 seconds.
/// Payload types and SSRCs are assigned dynamically by FFmpeg; receivers should probe
/// or be handed an SDP out of band.
class RtpMulticastSink final : public StreamSink {
public:
    explicit RtpMulticastSink(RtpMulticastSinkConfig config);
    ~RtpMulticastSink() override;

    QString describe() const override;

    bool open(const StreamFormat &format, QString *error) override;
    void close() override;
    bool isOpen() const override { return m_open; }

    bool writeVideo(const std::uint8_t *data, std::size_t size, std::uint32_t rtpTimestamp, bool isKeyframe) override;
    bool writeAudio(const std::uint8_t *data, std::size_t size, std::uint32_t rtpTimestamp) override;

    quint64 bytesWritten() const override { return m_bytesWritten.load(std::memory_order_relaxed); }

private:
    /// One single-stream FFmpeg rtp muxer context. The rtp muxer is single-stream only,
    /// so video and audio each get their own context and destination port.
    struct RtpStreamContext {
        AVFormatContext *format = nullptr;
        AVPacket *packet = nullptr;

        // 32-bit RTP timestamp unwrapping state (see TsMulticastSink for the rationale).
        qint64 lastRaw = -1;
        qint64 offset = 0;
        qint64 base = -1; // first absolute timestamp seen, rebased to zero
    };

    /// Opens one single-stream rtp muxer context. A VideoCodec of Unknown selects the
    /// Opus audio stream; H264/H265 select the video stream (FFmpeg's AVCodecID is a
    /// typedef'd enum and cannot be forward-declared, so it stays out of this header).
    static bool openStream(RtpStreamContext &stream, const QString &url, VideoCodec codec,
                           int width, int height, const std::uint8_t *extradata, std::size_t extradataSize,
                           int sampleRate, int channels, QString *error);
    static void closeStream(RtpStreamContext &stream);

    /// Unwraps a 32-bit RTP timestamp into an absolute one and rebases it to zero.
    static qint64 unwrapTimestamp(std::uint32_t raw, RtpStreamContext &stream);

    bool writePacket(RtpStreamContext &stream, const std::uint8_t *data, std::size_t size,
                     qint64 pts, bool isKeyframe);

    /// Builds the udp:// destination with the socket options FFmpeg's UDP protocol
    /// understands (pkt_size, ttl, localaddr, overrun_nonfatal, fifo_size).
    static QString buildUrl(const RtpMulticastSinkConfig &config, quint16 port);

    RtpMulticastSinkConfig m_config;
    RtpStreamContext m_video;
    RtpStreamContext m_audio;
    bool m_open = false;
    std::atomic<quint64> m_bytesWritten { 0 };
};

} // namespace CBridge