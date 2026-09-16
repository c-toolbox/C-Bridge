/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "models/streamdraft.h"
#include "models/streamlistmodel.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace CBridge {

class BridgeEngine;

/// The single object QML talks to. Owns the config document and drives the engine.
class BridgeController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString configPath READ configPath NOTIFY configChanged)
    Q_PROPERTY(QString configName READ configName NOTIFY configChanged)
    Q_PROPERTY(bool dirty READ isDirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(QString aggregateMbps READ aggregateMbps NOTIFY statsUpdated)
    Q_PROPERTY(QString aggregateOutMbps READ aggregateOutMbps NOTIFY statsUpdated)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(CBridge::StreamListModel *streams READ streams CONSTANT)
    Q_PROPERTY(CBridge::StreamDraft *draft READ draft CONSTANT)
    Q_PROPERTY(QStringList draftProblems READ draftProblems NOTIFY draftProblemsChanged)
    Q_PROPERTY(bool draftValid READ isDraftValid NOTIFY draftProblemsChanged)
    Q_PROPERTY(bool draftIsNew READ draftIsNew NOTIFY draftProblemsChanged)
    Q_PROPERTY(bool ndiAvailable READ isNdiAvailable CONSTANT)
    Q_PROPERTY(QString ndiStatus READ ndiStatus CONSTANT)
    Q_PROPERTY(QStringList sinkKindNames READ sinkKindNames CONSTANT)

public:
    explicit BridgeController(QObject *parent = nullptr);
    ~BridgeController() override;

    QString configPath() const { return m_configPath; }
    QString configName() const { return m_config.name; }
    bool isDirty() const { return m_dirty; }
    bool isRunning() const;
    QString aggregateMbps() const;
    QString aggregateOutMbps() const;
    QString statusMessage() const { return m_statusMessage; }
    StreamListModel *streams() const { return m_model; }
    StreamDraft *draft() const { return m_draft; }
    QStringList draftProblems() const { return m_draftProblems; }
    bool isDraftValid() const { return m_draftProblems.isEmpty(); }
    bool draftIsNew() const { return m_draftIsNew; }
    bool isNdiAvailable() const;
    QString ndiStatus() const;
    QStringList sinkKindNames() const;

    /// Applies the startup config selection: an explicit path wins, otherwise the
    /// last used one when auto-load is enabled.
    void loadStartupConfig(const QString &explicitPath);

    Q_INVOKABLE void newConfig();
    Q_INVOKABLE bool loadConfig(const QString &path);
    Q_INVOKABLE bool saveConfig(const QString &path);

    Q_INVOKABLE void startAll();
    Q_INVOKABLE void stopAll();
    Q_INVOKABLE void startStream(const QString &streamId);
    Q_INVOKABLE void stopStream(const QString &streamId);

    Q_INVOKABLE void addStream(const QString &name, const QString &whepUrl);
    Q_INVOKABLE void removeStream(const QString &streamId);
    Q_INVOKABLE void setStreamEnabled(const QString &streamId, bool enabled);

    /// Starts editing a detached copy. An empty id seeds a brand new stream.
    Q_INVOKABLE void beginEditStream(const QString &streamId);
    Q_INVOKABLE bool commitEdit();
    Q_INVOKABLE void cancelEdit();

    /// Fills a sink row with a multicast endpoint not already claimed in this config.
    Q_INVOKABLE void suggestMulticastFor(int sinkRow);

    /// The plain udp:// address of a stream's first enabled multicast sink, for pasting
    /// into a player such as mpv.
    Q_INVOKABLE QString tsAddressFor(const QString &streamId) const;

    Q_INVOKABLE QStringList validationProblems() const;

Q_SIGNALS:
    void configChanged();
    void dirtyChanged();
    void runningChanged();
    void statsUpdated();
    void statusMessageChanged();
    void draftProblemsChanged();

private:
    void setStatusMessage(const QString &message);
    void setDirty(bool dirty);
    void refreshModel();
    void recomputeDraftProblems();

    BridgeConfig m_config;
    QString m_configPath;
    bool m_dirty = false;
    QString m_statusMessage;

    BridgeEngine *m_engine = nullptr;
    StreamListModel *m_model = nullptr;
    StreamDraft *m_draft = nullptr;
    QStringList m_draftProblems;
    bool m_draftIsNew = true;
};

} // namespace CBridge
