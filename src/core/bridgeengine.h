/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "core/bridgetypes.h"

#include <QHash>
#include <QList>
#include <QObject>

class QTimer;

namespace CBridge {

class StreamPipeline;

/// Owns every running pipeline and exposes aggregate state to the UI.
class BridgeEngine : public QObject
{
    Q_OBJECT

public:
    explicit BridgeEngine(QObject *parent = nullptr);
    ~BridgeEngine() override;

    void setConfig(const BridgeConfig &config);
    const BridgeConfig &config() const { return m_config; }

    void startAll();
    void stopAll();
    void startStream(const QString &streamId);
    void stopStream(const QString &streamId);

    bool isRunning() const { return !m_pipelines.isEmpty(); }
    bool isStreamRunning(const QString &streamId) const { return m_pipelines.contains(streamId); }

    StreamStats statsFor(const QString &streamId) const;
    QList<SinkStats> sinkStatsFor(const QString &streamId) const;
    double aggregateInputMbps() const;
    double aggregateOutputMbps() const;

Q_SIGNALS:
    void streamStateChanged(const QString &streamId, CBridge::StreamState state);
    void streamError(const QString &streamId, const QString &message);
    void statsUpdated();

private:
    void sampleStats();

    BridgeConfig m_config;
    QHash<QString, StreamPipeline *> m_pipelines;
    QHash<QString, StreamStats> m_stats;
    QHash<QString, quint64> m_lastBytesIn;
    QHash<QString, QList<SinkStats>> m_sinkStats;

    QTimer *m_statsTimer = nullptr;
};

} // namespace CBridge
