/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QAbstractListModel>
#include <QThread>
#include <QVariantMap>

class QNetworkAccessManager;

namespace CBridge {

/// Holds the list of known MediaMTX servers, persisted in data/mediamtx-servers.json.
/// Manually entered passwords are deliberately kept in memory only and never written to
/// disk; alternatively a password can be picked up from the Windows Credential Manager.
class MediaMtxServersModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        HostRole,
        ApiPortRole,
        ApiSchemeRole,
        UsernameRole,
        WebRtcPortRole,
        WebRtcSchemeRole,
        AutoDetectWebRtcRole,
        EnabledRole,
        HasPasswordRole,
    };

    explicit MediaMtxServersModel(QObject *parent = nullptr);
    ~MediaMtxServersModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Reloads the server list from disk.
    Q_INVOKABLE void updateServersList();

    Q_PROPERTY(int numberOfServers READ getNumberOfServers NOTIFY serversListChanged)
    int getNumberOfServers() const;

    Q_INVOKABLE void addServer(const QString &name, const QString &host, int apiPort,
                               const QString &apiScheme, const QString &username,
                               int webRtcPort, const QString &webRtcScheme,
                               bool autoDetectWebRtc, bool enabled);
    Q_INVOKABLE void updateServer(int index, const QString &name, const QString &host, int apiPort,
                                  const QString &apiScheme, const QString &username,
                                  int webRtcPort, const QString &webRtcScheme,
                                  bool autoDetectWebRtc, bool enabled);
    Q_INVOKABLE void removeServer(int index);
    Q_INVOKABLE void moveServer(int from, int to);
    Q_INVOKABLE QVariantMap serverAt(int index) const;

    /// Session-only credential, cleared when C-Bridge exits.
    Q_INVOKABLE void setPassword(int index, const QString &password);
    Q_INVOKABLE bool hasPassword(int index) const;
    QString password(int index) const;

    /// Checks the Windows Credential Manager for a generic credential stored under this
    /// server's name and API user ("MediaMTX/<name>/<username>").
    Q_INVOKABLE bool hasStoredCredential(const QString &serverName, const QString &username) const;
    /// The password actually used to connect: the session (manually entered) password when
    /// set, otherwise the one found in the Windows Credential Manager.
    QString effectivePassword(int index) const;

    /// True when a password can be used right now for this server: either the session
    /// (manually entered) one or a stored credential from the Windows Credential Manager.
    Q_INVOKABLE bool hasUsablePassword(int index) const;

    QString apiBaseUrl(int index) const;
    QString username(int index) const;
    QString name(int index) const;
    /// Applies the WebRTC port/scheme detected from the server config, without touching the stored file.
    void applyDetectedWebRtc(int index, int port, const QString &scheme);

Q_SIGNALS:
    void serversListChanged();

private:
    struct Server {
        QString name;
        QString host;
        int apiPort = 9997;
        QString apiScheme = QStringLiteral("http");
        QString username;
        QString password; // session only
        int webRtcPort = 8889;
        QString webRtcScheme = QStringLiteral("http");
        bool autoDetectWebRtc = true;
        bool enabled = true;
    };

    void saveServersToFile();
    bool isValidIndex(int index) const;

    QList<Server> m_servers;
};

/// Runs the MediaMTX control API requests on a worker thread so the UI never blocks.
class MediaMtxWorker : public QObject
{
    Q_OBJECT

public:
    explicit MediaMtxWorker(QObject *parent = nullptr);

public Q_SLOTS:
    void doFetch(const QString &baseUrl, const QString &username, const QString &password,
                 bool includeConfiguredPaths, bool fetchGlobalConfig);

Q_SIGNALS:
    void fetchFinished(int statusCode, const QString &pathsJson, const QString &configPathsJson,
                       const QString &globalConfigJson, const QString &error);

private:
    struct Response {
        int status = 0;
        QByteArray body;
        QString error;
    };

    /// Blocking GET for this thread only; the worker runs a local event loop per request.
    Response get(const QUrl &url) const;

    QNetworkAccessManager *m_network = nullptr;
    QString m_username;
    QString m_password;
};

/// Lists the streams (paths) available on a MediaMTX server and turns them into WHEP URLs
/// that can be used by C-Bridge streams.
class MediaMtxModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        ServerNameRole,
        WhepUrlRole,
        OnlineRole,
        SourceTypeRole,
        TracksRole,
        ReadersRole,
        ConfiguredOnlyRole,
        RequiresAuthRole,
    };

    explicit MediaMtxModel(QObject *parent = nullptr);
    ~MediaMtxModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setServersModel(MediaMtxServersModel *servers);

    Q_INVOKABLE void refresh(int serverIndex);
    Q_INVOKABLE void testConnection(int serverIndex);
    Q_INVOKABLE void clear();

    Q_INVOKABLE QString whepUrlAt(int index, bool includeCredentials = false) const;
    Q_INVOKABLE QString nameAt(int index) const;

    /// True when the path's configuration requires read authentication
    /// ("readAuthentication: internal"), so fetching it needs credentials.
    Q_INVOKABLE bool requiresAuthAt(int index) const;

    Q_PROPERTY(int numberOfStreams READ getNumberOfStreams NOTIFY streamsListChanged)
    int getNumberOfStreams() const;

    Q_PROPERTY(int currentServerIndex READ currentServerIndex NOTIFY streamsListChanged)
    int currentServerIndex() const;

    Q_PROPERTY(bool refreshInProgress READ refreshInProgress NOTIFY refreshInProgressChanged)
    bool refreshInProgress() const;

    Q_PROPERTY(QString lastError READ lastError NOTIFY responseChanged)
    QString lastError() const;

    Q_PROPERTY(int lastStatusCode READ lastStatusCode NOTIFY responseChanged)
    int lastStatusCode() const;

    Q_PROPERTY(QString lastSummary READ lastSummary NOTIFY responseChanged)
    QString lastSummary() const;

Q_SIGNALS:
    void streamsListChanged();
    void refreshInProgressChanged();
    void responseChanged();
    void startFetch(const QString &baseUrl, const QString &username, const QString &password,
                    bool includeConfiguredPaths, bool fetchGlobalConfig);

private Q_SLOTS:
    void onFetchFinished(int statusCode, const QString &pathsJson, const QString &configPathsJson,
                         const QString &globalConfigJson, const QString &error);

private:
    struct Stream {
        QString name;
        QString serverName;
        QString whepUrl;
        bool online = false;
        QString sourceType;
        QString tracks;
        int readers = 0;
        bool configuredOnly = false;

        /// The path's readAuthentication is "internal", so the WHEP URL needs credentials.
        bool requiresAuth = false;
    };

    void applyGlobalConfig(const QString &globalConfigJson);
    QString buildWhepUrl(const QString &pathName, bool includeCredentials) const;

    MediaMtxServersModel *m_servers = nullptr;
    QList<Stream> m_streams;

    int m_currentServerIndex = -1;
    int m_pendingServerIndex = -1;
    bool m_testOnly = false;
    bool m_refreshInProgress = false;
    QString m_lastError;
    QString m_lastSummary;
    int m_lastStatusCode = 0;

    QThread m_workerThread;
    MediaMtxWorker *m_worker = nullptr;
};

} // namespace CBridge