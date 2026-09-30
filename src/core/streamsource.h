/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "core/bridgetypes.h"

#include <QByteArray>
#include <QObject>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace CBridge {

struct StreamConfig; // defined in config/bridgeconfig.h

/// Common interface for stream sources (WHEP/WebRTC, SRT).
///
/// Media is delivered on the source's own worker threads through plain std::function
/// callbacks rather than Qt signals: the hot path must not queue through the GUI event
/// loop. State changes do use signals and are safe to bind to the UI; they may be
/// emitted from a non-GUI thread, so connections are queued automatically.
class StreamSource : public QObject
{
    Q_OBJECT

public:
    /// data points at an Annex-B access unit (video) or an Opus packet (audio).
    /// It is only valid for the duration of the call; copy it if you need to keep it.
    using MediaFrameCallback =
        std::function<void(const std::uint8_t *data, std::size_t size, quint32 rtpTimestamp)>;

    explicit StreamSource(QObject *parent = nullptr);
    ~StreamSource() override;

    /// Configures the source before start(). The config is copied and must not be mutated
    /// afterwards.
    virtual void setConfig(const StreamConfig &config) = 0;

    void setVideoCallback(MediaFrameCallback callback);
    void setAudioCallback(MediaFrameCallback callback);

    /// Source credentials (WHEP username/password). A no-op for sources that carry
    /// their own authentication in the config, such as SRT passphrases.
    virtual void setPassword(const QString &password) { Q_UNUSED(password); }

    /// Starts the source. Calling start while a previous run is still winding down
    /// waits for it to finish first, so runs never overlap.
    virtual void start() = 0;

    /// Stops the source and returns once its worker thread has finished. Safe from any
    /// thread; the stop must be noticed by the worker within a bounded time.
    virtual void stop() = 0;

    /// The video codec the source currently delivers (Unknown until negotiated).
    virtual VideoCodec negotiatedVideoCodec() const { return VideoCodec::Unknown; }

    /// The audio codec the source delivers (Unknown until negotiated). Sources that only
    /// ever carry one audio format may leave the default.
    virtual AudioCodec negotiatedAudioCodec() const { return AudioCodec::Unknown; }

    /// Codec extradata for the negotiated audio (AudioSpecificConfig for AAC, empty for
    /// Opus). The default is empty, which is correct for Opus.
    virtual QByteArray negotiatedAudioExtradata() const { return {}; }

    /// Sample rate and channel count of the negotiated audio. Opus is always 48 kHz, but
    /// AAC may be 44.1 kHz or mono, and the muxers must be told the real values.
    virtual int negotiatedAudioSampleRate() const { return 48000; }
    virtual int negotiatedAudioChannels() const { return 2; }

    StreamState state() const;

Q_SIGNALS:
    void stateChanged(CBridge::StreamState state);
    void videoCodecNegotiated(CBridge::VideoCodec codec);
    /// A problem worth surfacing in the UI. It may or may not be followed by a move to
    /// Failed, depending on whether the source gives up.
    void errorOccurred(const QString &message);

protected:
    /// Updates the shared state and emits stateChanged when it actually changed.
    void setState(StreamState state);

    MediaFrameCallback m_onVideo;
    MediaFrameCallback m_onAudio;

private:
    std::atomic<StreamState> m_state { StreamState::Idle };
};

} // namespace CBridge