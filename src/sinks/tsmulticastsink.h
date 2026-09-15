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

struct AVCodecParameters;
struct AVFormatContext;
struct AVPacket;
struct AVStream;

namespace CBridge {

/// Muxes the incoming elementary streams into MPEG-TS and sends them to a UDP
/// multicast group.
///
/// This path is a pure passthrough: the H.264/H.265 bitstream and the Opus packets
/// are written exactly as received, so no decoder or encoder is involved. The RTP
/// video clock is already 90 kHz, which is the MPEG-TS timebase, so video
/// timestamps need no rescaling at all.
class TsMulticastSink final : public StreamSink
{
public:
    explicit TsMulticastSink(TsMulticastSinkConfig config);
    ~TsMulticastSink() override;

    QString describe() const override;

    bool open(const StreamFormat &format, QString *error) override;
    void close() override;
    bool isOpen() const override { return m_open; }

    bool writeVideo(const std::uint8_t *data, std::size_t size,
                    std::uint32_t rtpTimestamp, bool isKeyframe) override;
    bool writeAudio(const std::uint8_t *data, std::size_t size,
                    std::uint32_t rtpTimestamp) override;

    quint64 bytesWritten() const override
    {
        return m_bytesWritten.load(std::memory_order_relaxed);
    }

private:
    bool addVideoStream(const StreamFormat &format, QString *error);
    bool addAudioStream(const StreamFormat &format, QString *error);

    /// Converts a wrapping 32-bit RTP timestamp into a monotonic 64-bit PTS.
    qint64 unwrap(std::uint32_t rtpTimestamp, qint64 &lastRaw, qint64 &offset) const;

    bool writePacket(AVStream *stream, const std::uint8_t *data, std::size_t size,
                     qint64 pts, bool isKeyframe);

    TsMulticastSinkConfig m_config;

    AVFormatContext *m_format = nullptr;
    AVStream *m_videoStream = nullptr;
    AVStream *m_audioStream = nullptr;
    AVPacket *m_packet = nullptr;

    bool m_open = false;

    qint64 m_videoLastRaw = -1;
    qint64 m_videoOffset = 0;
    qint64 m_audioLastRaw = -1;
    qint64 m_audioOffset = 0;

    /// Both streams are anchored to the first video timestamp so they share a zero.
    qint64 m_ptsBase = -1;

    std::atomic<quint64> m_bytesWritten { 0 };
};

} // namespace CBridge
