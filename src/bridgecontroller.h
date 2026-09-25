/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "mediamtxmodel.h"
#include "models/streamdraft.h"
#include "models/streamlistmodel.h"

#include <QObject>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>

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
    Q_PROPERTY(CBridge::MediaMtxServersModel *mediaMtxServers READ mediaMtxServers CONSTANT)
    Q_PROPERTY(CBridge::MediaMtxModel *mediaMtxStreams READ mediaMtxStreams CONSTANT)
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
    MediaMtxServersModel *mediaMtxServers() const { return m_mediaMtxServers; }
    MediaMtxModel *mediaMtxStreams() const { return m_mediaMtxStreams; }
    QStringList draftProblems() const { return m_draftProblems; }
    bool isDraftValid() const { return m_draftProblems.isEmpty(); }
    bool draftIsNew() const { return m_draftIsNew; }
    bool isNdiAvailable() const;
    QString ndiStatus() const;
    QStringList sinkKindNames() const;

    /// Applies the startup config selection: an explicit path wins, then the configured
    /// startup document, then the last used one when auto-load is enabled. Streams flagged
    /// to start on load are started once a configuration has been loaded.
    void loadStartupConfig(const QString &explicitPath);

    Q_INVOKABLE void newConfig();
    Q_INVOKABLE bool loadConfig(const QString &path);
    Q_INVOKABLE bool saveConfig(const QString &path);

    Q_INVOKABLE void startAll();
    Q_INVOKABLE void stopAll();
    Q_INVOKABLE void startStream(const QString &streamId);
    Q_INVOKABLE void stopStream(const QString &streamId);

    Q_INVOKABLE void addStream(const QString &name, const QString &whepUrl);
    /// Adds the MediaMTX stream at `streamIndex` as a new entry in this configuration. When
    /// `includeCredentials` is set the server's user:password is embedded in the WHEP URL;
    /// otherwise only the username is stored and the password stays out of the document. A
    /// path that requires read authentication always gets credentials appended, since it could
    /// not be fetched without them; adding fails with a status message when no API user or
    /// password is available for its server.
    Q_INVOKABLE bool addMediaMtxStream(int streamIndex, const QString &title, bool includeCredentials);
    Q_INVOKABLE void removeStream(const QString &streamId);
    Q_INVOKABLE void setStreamEnabled(const QString &streamId, bool enabled);

    /// Session-only password for a stream entry, keyed by id and never written to the
    /// configuration document. It is used when starting streams whose URL carries no embedded
    /// credentials; an empty string clears it again.
    Q_INVOKABLE void setStreamPassword(const QString &streamId, const QString &password);
    Q_INVOKABLE bool hasStreamPassword(const QString &streamId) const;
    Q_INVOKABLE QString streamPassword(const QString &streamId) const;

    /// True when the Windows Credential Manager holds an entry for the MediaMTX server this
    /// WHEP URL points at, under this username. The editor shows it so a stored password is
    /// visible outside the MediaMTX dialog too.
    Q_INVOKABLE bool hasStoredCredentialFor(const QString &whepUrl, const QString &username) const;

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

    /// The per-stream "start on configuration load" flags of the current document as
    /// {id, name, autoStart} rows, for the preferences dialog.
    Q_INVOKABLE QVariantList streamAutoStarts() const;

    /// Persists the startup preferences and the per-stream auto-start flags in one save.
    Q_INVOKABLE void saveStartupSettings(const QString &configPath, bool autoLoadLastConfig,
                                         bool startOnLoad, const QVariantList &streamRows);

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

    /// Starts the enabled streams flagged to run on load (global switch or per stream).
    void startStreamsOnLoad();
    bool streamAutoStart(const QString &streamId) const;

    /// The index of the MediaMTX server whose WebRTC endpoint (host + port) matches this URL,
    /// or -1 when none does.
    int matchingServerIndex(const QUrl &url) const;

    /// The password used to start a stream whose URL carries no embedded credentials: the one
    /// entered for this session, else the Windows Credential Manager entry of the matching
    /// server and user.
    QString resolvedStreamPassword(const StreamConfig &stream) const;

    /// Pushes every stream's resolved password into the engine before starting.
    void applyStreamPasswords();

    BridgeConfig m_config;
    QString m_configPath;
    bool m_dirty = false;
    QString m_statusMessage;

    BridgeEngine *m_engine = nullptr;
    StreamListModel *m_model = nullptr;
    StreamDraft *m_draft = nullptr;
    MediaMtxServersModel *m_mediaMtxServers = nullptr;
    MediaMtxModel *m_mediaMtxStreams = nullptr;
    QStringList m_draftProblems;
    bool m_draftIsNew = true;

    /// Session-only stream passwords keyed by stream id, cleared when C-Bridge exits.
    QHash<QString, QString> m_streamPasswords;
};

} // namespace CBridge
