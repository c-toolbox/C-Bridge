/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "media/avwrappers.h"

#include <QString>

#include <cstdint>
#include <functional>

namespace CBridge {

/// Decodes Opus packets to planar float, which is already NDI's audio layout.
class AudioDecoder
{
public:
    using FrameCallback = std::function<void(AVFrame *frame)>;

    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;

    bool open(int sampleRate, int channels, QString *error);
    void close();
    bool isOpen() const { return m_context != nullptr; }

    int sampleRate() const;
    int channels() const;

    /// Name of the FFmpeg decoder actually in use ("libopus" when available, otherwise "opus").
    QString decoderName() const { return m_decoderName; }

    void setFrameCallback(FrameCallback callback) { m_onFrame = std::move(callback); }

    bool decode(const std::uint8_t *data, std::size_t size, std::int64_t pts, QString *error);

    /// Number of packets that only decoded after dropping one trailing byte (see decode()).
    /// Diagnostic: a high count means the upstream encoder is sending malformed Opus payloads.
    std::uint64_t trailingByteRetryCount() const { return m_trailingByteRetries; }

private:
    /// Feeds one packet to FFmpeg and delivers every resulting frame through the callback.
    bool decodePacket(const std::uint8_t *data, std::size_t size, std::int64_t pts, QString *error);

    /// Receives and delivers at most one decoded frame (converting it to planar float when the
    /// decoder did not already emit AV_SAMPLE_FMT_FLTP). Returns 1 if a frame was delivered,
    /// 0 if no frame is pending right now, or -1 with *error set on failure.
    int receiveAndDeliverFrame(QString *error);

    /// Returns a planar-float view of frame, converting it when the decoder did not already
    /// emit AV_SAMPLE_FMT_FLTP (the libopus wrapper emits packed s16 in this FFmpeg build).
    AVFrame *toPlanarFloat(AVFrame *frame, QString *error);

    CodecContextPtr m_context;
    QString m_decoderName;
    std::uint64_t m_trailingByteRetries = 0;
    FramePtr m_frame;
    PacketPtr m_packet;
    FrameCallback m_onFrame;

    /// Guarantees the planar-float contract NDI relies on. Reconfigured only when the decoded
    /// frame's format, channel count or sample rate changes, which for Opus is stable.
    ResamplerPtr m_resampler;
    FramePtr m_fltpFrame;
    int m_resampleInFormat = -1;
    int m_resampleInChannels = 0;
    int m_resampleInRate = 0;
};

} // namespace CBridge
