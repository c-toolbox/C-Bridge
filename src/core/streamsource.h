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
    ///
    /// Video carries two clocks on the 90 kHz grid: dtsTimestamp in decode order (what a
    /// muxer interleaves and must keep monotonic) and ptsTimestamp in presentation order
    /// (what a player displays). They are equal for B-frame-free streams; with B-frames —
    /// which YouTube's HLS avc1 formats carry — they differ by the reordering depth, and
    /// only keeping them apart lets sinks emit containers whose display timeline is
    /// monotonic.
    using MediaFrameCallback = std::function<void(const std::uint8_t *data, std::size_t size,
                                                  quint32 dtsTimestamp, quint32 ptsTimestamp)>;

    /// Audio has no decode/display split: one clock on the sample-rate grid (48 kHz for
    /// Opus), so the audio callback keeps a single-timestamp shape.
    using AudioFrameCallback = std::function<void(const std::uint8_t *data, std::size_t size,
                                                  quint32 timestamp)>;

    explicit StreamSource(QObject *parent = nullptr);
    ~StreamSource() override;

    /// Configures the source before start(). The config is copied and must not be mutated
    /// afterwards.
    virtual void setConfig(const StreamConfig &config) = 0;

    void setVideoCallback(MediaFrameCallback callback);
    void setAudioCallback(AudioFrameCallback callback);

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

    // --- Playback control -----------------------------------------------------
    // Optional per-source capability, following the setPassword() precedent: WHEP and SRT
    // keep the defaults and are never asked; only sources that can actually control their
    // media timeline (the yt-dlp VOD source) override these. The commands are user-driven
    // and rare, so they are plain calls from the GUI thread into the source's atomics —
    // the same thread-safety contract stop() already relies on. Position and duration are
    // deliberately polled (via StreamStats), not signalled, to avoid per-frame signals.

    /// True when this source supports pause/resume/seek (YouTube VOD). The UI shows
    /// transport controls only when a running stream's source answers true.
    virtual bool isPlaybackControllable() const { return false; }

    /// True when the media timeline is live: seeking makes no sense and the UI hides the
    /// seek slider. Default false (a source that never reports live is treated as VOD).
    virtual bool isLive() const { return false; }

    /// Suspend media delivery without tearing the source down. Default: no-op.
    virtual void requestPause() {}

    /// Resume delivery after requestPause(). Default: no-op.
    virtual void requestResume() {}

    /// Jump to an absolute media position in milliseconds. Default: no-op. Implementations
    /// that cannot seek (live streams) ignore the request.
    virtual void requestSeek(qint64 positionMs) { Q_UNUSED(positionMs); }

    /// Current playback position in seconds (0 for non-controllable sources). Polled.
    virtual double mediaPositionSeconds() const { return 0.0; }

    /// Total media duration in seconds (0 when unknown or live). Polled.
    virtual double mediaDurationSeconds() const { return 0.0; }

    /// Returns true once after the source restarted its media timeline in place (a seeked
    /// yt-dlp child generation), consuming the flag. The pipeline re-arms its keyframe gate
    /// when it sees one, so sinks wait for the new generation's first IDR exactly as they do
    /// after a reconnect. Default false: sources that never restart internally.
    virtual bool takeGenerationRestartPending() { return false; }

    /// One-line diagnostic of the source's last notable recovery event ("resumed at 123.4 s
    /// after stall", "direct-url", "pipe fallback"), for the UI's stats row. Default empty:
    /// sources without recovery behaviour have nothing to report.
    virtual QString lastEvent() const { return {}; }

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
    AudioFrameCallback m_onAudio;

private:
    std::atomic<StreamState> m_state { StreamState::Idle };
};

} // namespace CBridge