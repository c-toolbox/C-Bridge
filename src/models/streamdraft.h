/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "models/sinklistmodel.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace CBridge {

/// A detached, editable copy of a StreamConfig. Cancel is real because nothing
/// here is written back until the controller commits it.
class StreamDraft : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString streamId READ streamId NOTIFY changed)
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QString whepUrl READ whepUrl WRITE setWhepUrl NOTIFY whepUrlChanged)
    Q_PROPERTY(QString username READ username WRITE setUsername NOTIFY usernameChanged)
    Q_PROPERTY(bool audioEnabled READ isAudioEnabled WRITE setAudioEnabled NOTIFY audioEnabledChanged)
    Q_PROPERTY(QStringList preferredCodecs READ preferredCodecs WRITE setPreferredCodecs NOTIFY preferredCodecsChanged)
    Q_PROPERTY(int reconnectInitialMs READ reconnectInitialMs WRITE setReconnectInitialMs NOTIFY reconnectInitialMsChanged)
    Q_PROPERTY(int reconnectMaxMs READ reconnectMaxMs WRITE setReconnectMaxMs NOTIFY reconnectMaxMsChanged)
    Q_PROPERTY(CBridge::SinkListModel *sinks READ sinks CONSTANT)

public:
    explicit StreamDraft(QObject *parent = nullptr);

    QString streamId() const { return m_config.id; }
    QString name() const { return m_config.name; }
    bool isEnabled() const { return m_config.enabled; }
    QString whepUrl() const { return m_config.whepUrl.toString(); }
    QString username() const { return m_config.username; }
    bool isAudioEnabled() const { return m_config.audioEnabled; }
    QStringList preferredCodecs() const;
    int reconnectInitialMs() const { return m_config.reconnectInitialMs; }
    int reconnectMaxMs() const { return m_config.reconnectMaxMs; }
    SinkListModel *sinks() const { return m_sinks; }

    void setName(const QString &name);
    void setEnabled(bool enabled);
    void setWhepUrl(const QString &url);
    void setUsername(const QString &username);
    void setAudioEnabled(bool enabled);
    void setPreferredCodecs(const QStringList &codecs);
    void setReconnectInitialMs(int ms);
    void setReconnectMaxMs(int ms);

    void load(const StreamConfig &config);
    StreamConfig toConfig() const;

Q_SIGNALS:
    void nameChanged();
    void enabledChanged();
    void whepUrlChanged();
    void usernameChanged();
    void audioEnabledChanged();
    void preferredCodecsChanged();
    void reconnectInitialMsChanged();
    void reconnectMaxMsChanged();

    /// Fired alongside every specific NOTIFY so validation recomputes once.
    void changed();

private:
    StreamConfig m_config;
    SinkListModel *m_sinks = nullptr;
};

} // namespace CBridge
