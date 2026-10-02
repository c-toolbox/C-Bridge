/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "core/bridgetypes.h"
#include "core/mediaqueue.h"
#include "media/bitstreaminspector.h"

#include <QList>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace CBridge {

class StreamSink;
class StreamSource;
class StreamPreview;

/// Runs one stream end to end: source in (WHEP or SRT), N sinks out.
///
/// The network thread only copies units into a bounded queue. A single worker
/// thread drains it and drives every sink, so a slow sink can never stall the
/// transport.
class StreamPipeline : public QObject
{
    Q_OBJECT

public:
    explicit StreamPipeline(StreamConfig config, QObject *parent = nullptr);
    ~StreamPipeline() override;

    const StreamConfig &config() const { return m_config; }
    QString id() const { return m_config.id; }

    /// The transport this pipeline pulls from (SRT or WHEP). Only valid between start() and
    /// stop(); exposed for diagnostics and tests.
    StreamSource *source() const { return m_source; }

    /// This stream's built-in viewer, created with the pipeline and owned by it. It stays
    /// inert (no decode, no audio device) until a viewer window attaches through
    /// StreamPreview::setActive(). Never null once the pipeline exists.
    StreamPreview *preview() const { return m_preview; }

    // --- Playback control ---------------------------------------------------------------
    // Forwarded to the source, which decides whether it can honour them. Safe from the GUI
    // thread: the source implements these as plain atomic flags (see StreamSource).
    bool isPlaybackControllable() const;
    bool isLive() const;
    void requestPause();
    void requestResume();
    void requestSeek(qint64 positionMs);

    void setPassword(const QString &password);

    void start();
    void stop();

    StreamStats stats() const;
    QList<SinkStats> sinkStats() const;

Q_SIGNALS:
    void stateChanged(const QString &streamId, CBridge::StreamState state);
    void errorOccurred(const QString &streamId, const QString &message);

private:
    void workerLoop();
    void handleVideoUnit(const MediaUnit &unit);
    void handleAudioUnit(const MediaUnit &unit);
    bool openSinks();
    void closeSinks();
    void scheduleReconnect();
    void setState(StreamState state);
    void updateSinkStats();

    StreamConfig m_config;
    QString m_password;

    StreamSource *m_source = nullptr;
    StreamPreview *m_preview = nullptr;
    std::vector<std::unique_ptr<StreamSink>> m_sinks;

    MediaQueue m_queue;
    std::thread m_worker;
    std::atomic_bool m_running { false };

    BitstreamInspector m_inspector;
    bool m_sinksOpen = false;
    bool m_sawKeyframe = false;

    /// Set by the GUI thread's scheduleReconnect() and consumed by the worker's pump loop,
    /// which owns the sinks: the worker closes them and re-arms the keyframe gate on its own
    /// thread, so the GUI thread never touches a sink a worker might still be writing to.
    std::atomic_bool m_pendingGateReset { false };

    /// True while the negotiated audio is Opus, so the Opus payload sanitizer runs. AAC
    /// packets are passed through untouched (the sanitizer's checks are RFC 6716 framing).
    bool m_audioIsOpus = true;

    /// Set once when upstream Opus payloads are found to carry undeclared trailing bytes, so the
    /// diagnostic warning fires a single time per stream instead of on every packet.
    std::atomic_bool m_warnedOpusTrailingBytes { false };

    /// SPS/PPS arrive as their own access units, which are dropped before the first
    /// keyframe, so they are cached and prepended to every keyframe that lacks them.
    std::vector<std::uint8_t> m_parameterSets;
    bool m_parameterSetsConsumed = false;
    std::vector<std::uint8_t> m_assembledUnit;

    QTimer *m_reconnectTimer = nullptr;
    int m_reconnectDelayMs = 0;

    mutable std::mutex m_statsMutex;
    StreamStats m_stats;
    QList<SinkStats> m_sinkStats;
};

} // namespace CBridge
