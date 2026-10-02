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
class StreamPreview;

/// Owns every running pipeline and exposes aggregate state to the UI.
class BridgeEngine : public QObject
{
    Q_OBJECT

public:
    explicit BridgeEngine(QObject *parent = nullptr);
    ~BridgeEngine() override;

    void setConfig(const BridgeConfig &config);
    const BridgeConfig &config() const { return m_config; }

    /// Session-only password for a stream whose URL carries no embedded credentials. The
    /// controller resolves it (entered in the editor, or picked up from the Windows
    /// Credential Manager) and pushes it here before starting the stream.
    void setStreamPassword(const QString &streamId, const QString &password);

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

    /// The built-in viewer of a running stream, or nullptr when the stream is not running.
    /// The viewer window binds to it and attaches via StreamPreview::setActive().
    StreamPreview *previewFor(const QString &streamId) const;

    // --- Playback control (forwarded to the running pipeline's source) ------------------
    bool isPlaybackControllable(const QString &streamId) const;
    bool isLive(const QString &streamId) const;
    void requestPause(const QString &streamId);
    void requestResume(const QString &streamId);
    void requestSeek(const QString &streamId, qint64 positionMs);

Q_SIGNALS:
    void streamStateChanged(const QString &streamId, CBridge::StreamState state);
    void streamError(const QString &streamId, const QString &message);
    void statsUpdated();

private:
    void sampleStats();

    BridgeConfig m_config;
    QHash<QString, StreamPipeline *> m_pipelines;
    /// Session-only passwords keyed by stream id; never persisted.
    QHash<QString, QString> m_streamPasswords;
    QHash<QString, StreamStats> m_stats;
    QHash<QString, quint64> m_lastBytesIn;
    QHash<QString, QList<SinkStats>> m_sinkStats;

    QTimer *m_statsTimer = nullptr;
};

} // namespace CBridge
