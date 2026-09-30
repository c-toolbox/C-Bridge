/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "core/streamsource.h"

#include <QByteArray>
#include <QObject>

#include <atomic>
#include <cstdint>
#include <thread>

struct AVFormatContext;

namespace CBridge {

/// Pulls one SRT stream (MPEG-TS) through FFmpeg's srt:// protocol.
///
/// The source runs its own read thread: avformat_open_input() blocks in accept
/// (listener mode) or connect (caller mode), and av_read_frame() blocks between
/// packets, so it cannot share a thread with anything else. Both waits are
/// interruptible through the AVFormatContext interrupt callback, which checks the
/// stop flag, so stop() returns within about 100 ms no matter where the source is
/// blocked.
class SrtSource : public StreamSource
{
    Q_OBJECT

public:
    explicit SrtSource(QObject *parent = nullptr);
    ~SrtSource() override;

    void setConfig(const StreamConfig &config) override;

    void start() override;
    void stop() override;

    VideoCodec negotiatedVideoCodec() const override;
    AudioCodec negotiatedAudioCodec() const override;
    QByteArray negotiatedAudioExtradata() const override;
    int negotiatedAudioSampleRate() const override;
    int negotiatedAudioChannels() const override;

private:
    /// Worker thread body. Opens the input, identifies the streams and pumps packets
    /// until a read error/EOF or a stop request.
    bool run();

    static int interruptCallback(void *opaque);

    /// Synthesises the AudioSpecificConfig FFmpeg's RTP muxer requires for AAC from the
    /// sample rate and channel count of the ADTS stream (the profile is fixed to AAC-LC,
    /// which every SRT encoder in practice uses). Empty for an unsupported rate.
    static QByteArray buildAacSpecificConfig(int sampleRate, int channels);

    SrtSourceConfig m_config;

    std::atomic<VideoCodec> m_videoCodec { VideoCodec::Unknown };
    std::atomic<AudioCodec> m_audioCodec { AudioCodec::Unknown };
    std::atomic<int> m_audioSampleRate { 48000 };
    std::atomic<int> m_audioChannels { 2 };
    QByteArray m_audioExtradata;
    std::thread m_thread;
    std::atomic<bool> m_stop { false };
};

} // namespace CBridge