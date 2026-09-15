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
class WebRtcSource;

/// Runs one stream end to end: WHEP source in, N sinks out.
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

    WebRtcSource *m_source = nullptr;
    std::vector<std::unique_ptr<StreamSink>> m_sinks;

    MediaQueue m_queue;
    std::thread m_worker;
    std::atomic_bool m_running { false };

    BitstreamInspector m_inspector;
    bool m_sinksOpen = false;
    bool m_sawKeyframe = false;

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
