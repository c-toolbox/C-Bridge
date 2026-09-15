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

    void setFrameCallback(FrameCallback callback) { m_onFrame = std::move(callback); }

    bool decode(const std::uint8_t *data, std::size_t size, std::int64_t pts, QString *error);

private:
    CodecContextPtr m_context;
    FramePtr m_frame;
    PacketPtr m_packet;
    FrameCallback m_onFrame;
};

} // namespace CBridge
