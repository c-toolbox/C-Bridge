/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mediamtxcredentials.h"
#include "mediamtxmodel.h"

#include "cbridgeversion.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

/// Resolves data/mediamtx-servers.json, searching in the same order as
/// BridgeApplication::dataPath() so the file lands next to the other runtime data whether
/// C-Bridge runs from an install folder or a build tree. Candidates that actually contain the
/// file are preferred over bare directory checks: without that pass, an unrelated data/ folder
/// in a build tree (CMake leftovers, log output) would shadow the real one and the list would
/// never be found.
QString serversFilePath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + u"/data"_s,
        appDir + u"/../data"_s,
        appDir + u"/../../data"_s,
        QString::fromUtf8(CBRIDGE_SOURCE_DATA_DIR),
    };

    for (const QString &base : candidates) {
        if (QFileInfo::exists(base + u"/mediamtx-servers.json"_s)) {
            return QDir::cleanPath(base + u"/mediamtx-servers.json"_s);
        }
    }

    // No file anywhere yet: create the fresh list in the first existing data directory.
    for (const QString &base : candidates) {
        if (QFileInfo::exists(base)) {
            return QDir::cleanPath(base + u"/mediamtx-servers.json"_s);
        }
    }

    return QDir::cleanPath(appDir + u"/data/mediamtx-servers.json"_s);
}

QString percentEncode(const QString &value)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(value));
}

// MediaMTX addresses are given as "host:port" or ":port".
int portFromAddress(const QString &address, int fallback)
{
    const int colon = address.lastIndexOf(QLatin1Char(':'));
    if (colon < 0 || colon == address.size() - 1) {
        return fallback;
    }
    bool ok = false;
    const int port = address.mid(colon + 1).toInt(&ok);
    return (ok && port > 0) ? port : fallback;
}

} // namespace

// ---------------------------------------------------------------------------
// MediaMtxServersModel
// ---------------------------------------------------------------------------

MediaMtxServersModel::MediaMtxServersModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // Load the persisted list at startup so it is available as soon as C-Bridge runs. The
    // MediaMTX dialog reloads on open to pick up edits made while the app is running.
    updateServersList();
}

MediaMtxServersModel::~MediaMtxServersModel() = default;

int MediaMtxServersModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return int(m_servers.size());
}

QVariant MediaMtxServersModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !checkIndex(index)) {
        return {};
    }

    const Server &s = m_servers.at(index.row());
    switch (role) {
    case NameRole:
        return s.name;
    case HostRole:
        return s.host;
    case ApiPortRole:
        return s.apiPort;
    case ApiSchemeRole:
        return s.apiScheme;
    case UsernameRole:
        return s.username;
    case WebRtcPortRole:
        return s.webRtcPort;
    case WebRtcSchemeRole:
        return s.webRtcScheme;
    case AutoDetectWebRtcRole:
        return s.autoDetectWebRtc;
    case EnabledRole:
        return s.enabled;
    case HasPasswordRole:
        return !s.password.isEmpty();
    default:
        return {};
    }
}

QHash<int, QByteArray> MediaMtxServersModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { HostRole, "host" },
        { ApiPortRole, "apiPort" },
        { ApiSchemeRole, "apiScheme" },
        { UsernameRole, "username" },
        { WebRtcPortRole, "webRtcPort" },
        { WebRtcSchemeRole, "webRtcScheme" },
        { AutoDetectWebRtcRole, "autoDetectWebRtc" },
        { EnabledRole, "enabled" },
        { HasPasswordRole, "hasPassword" },
    };
}

void MediaMtxServersModel::updateServersList()
{
    QFile serversFile(serversFilePath());
    if (!serversFile.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(serversFile.readAll());
    serversFile.close();
    if (!doc.isObject()) {
        qWarning("Ignoring %s: not a JSON object.", qPrintable(serversFilePath()));
        return;
    }

    const QJsonArray arr = doc.object().value(u"servers"_s).toArray();

    // Keep already entered session passwords when the list is reloaded.
    QHash<QString, QString> previousPasswords;
    for (const Server &s : std::as_const(m_servers)) {
        if (!s.password.isEmpty()) {
            previousPasswords.insert(s.name, s.password);
        }
    }

    beginResetModel();
    m_servers.clear();
    for (const QJsonValue &value : arr) {
        const QJsonObject o = value.toObject();
        Server s;
        s.name = o.value(u"name"_s).toString();
        s.host = o.value(u"host"_s).toString();
        s.apiPort = o.value(u"apiPort"_s).toInt(9997);
        s.apiScheme = o.value(u"apiScheme"_s).toString(u"http"_s);
        s.username = o.value(u"username"_s).toString();
        s.webRtcPort = o.value(u"webRtcPort"_s).toInt(8889);
        s.webRtcScheme = o.value(u"webRtcScheme"_s).toString(u"http"_s);
        s.autoDetectWebRtc = o.value(u"autoDetectWebRtc"_s).toBool(true);
        s.enabled = o.value(u"enabled"_s).toBool(true);
        if (previousPasswords.contains(s.name)) {
            s.password = previousPasswords.value(s.name);
        }
        if (!s.name.isEmpty() && !s.host.isEmpty()) {
            m_servers.append(s);
        }
    }
    endResetModel();
    qInfo() << "Loaded" << m_servers.size() << "MediaMTX server(s) from" << serversFilePath();
    Q_EMIT serversListChanged();
}

int MediaMtxServersModel::getNumberOfServers() const
{
    return int(m_servers.size());
}

bool MediaMtxServersModel::isValidIndex(int index) const
{
    return index >= 0 && index < m_servers.size();
}

void MediaMtxServersModel::addServer(const QString &name, const QString &host, int apiPort,
                                     const QString &apiScheme, const QString &username,
                                     int webRtcPort, const QString &webRtcScheme,
                                     bool autoDetectWebRtc, bool enabled)
{
    Server s;
    s.name = name;
    s.host = host;
    s.apiPort = apiPort > 0 ? apiPort : 9997;
    s.apiScheme = apiScheme.isEmpty() ? u"http"_s : apiScheme;
    s.username = username;
    s.webRtcPort = webRtcPort > 0 ? webRtcPort : 8889;
    s.webRtcScheme = webRtcScheme.isEmpty() ? u"http"_s : webRtcScheme;
    s.autoDetectWebRtc = autoDetectWebRtc;
    s.enabled = enabled;

    beginInsertRows(QModelIndex(), int(m_servers.size()), int(m_servers.size()));
    m_servers.append(s);
    endInsertRows();

    saveServersToFile();
    Q_EMIT serversListChanged();
}

void MediaMtxServersModel::updateServer(int index, const QString &name, const QString &host, int apiPort,
                                        const QString &apiScheme, const QString &username,
                                        int webRtcPort, const QString &webRtcScheme,
                                        bool autoDetectWebRtc, bool enabled)
{
    if (!isValidIndex(index)) {
        return;
    }

    Server &s = m_servers[index];
    s.name = name;
    s.host = host;
    s.apiPort = apiPort > 0 ? apiPort : 9997;
    s.apiScheme = apiScheme.isEmpty() ? u"http"_s : apiScheme;
    s.username = username;
    s.webRtcPort = webRtcPort > 0 ? webRtcPort : 8889;
    s.webRtcScheme = webRtcScheme.isEmpty() ? u"http"_s : webRtcScheme;
    s.autoDetectWebRtc = autoDetectWebRtc;
    s.enabled = enabled;

    Q_EMIT dataChanged(this->index(index, 0), this->index(index, 0));
    saveServersToFile();
    Q_EMIT serversListChanged();
}

void MediaMtxServersModel::removeServer(int index)
{
    if (!isValidIndex(index)) {
        return;
    }

    beginRemoveRows(QModelIndex(), index, index);
    m_servers.removeAt(index);
    endRemoveRows();

    saveServersToFile();
    Q_EMIT serversListChanged();
}

void MediaMtxServersModel::moveServer(int from, int to)
{
    if (!isValidIndex(from) || !isValidIndex(to) || from == to) {
        return;
    }

    beginResetModel();
    m_servers.move(from, to);
    endResetModel();

    saveServersToFile();
    Q_EMIT serversListChanged();
}

QVariantMap MediaMtxServersModel::serverAt(int index) const
{
    QVariantMap map;
    if (!isValidIndex(index)) {
        return map;
    }

    const Server &s = m_servers.at(index);
    map.insert(u"name"_s, s.name);
    map.insert(u"host"_s, s.host);
    map.insert(u"apiPort"_s, s.apiPort);
    map.insert(u"apiScheme"_s, s.apiScheme);
    map.insert(u"username"_s, s.username);
    map.insert(u"webRtcPort"_s, s.webRtcPort);
    map.insert(u"webRtcScheme"_s, s.webRtcScheme);
    map.insert(u"autoDetectWebRtc"_s, s.autoDetectWebRtc);
    map.insert(u"enabled"_s, s.enabled);
    map.insert(u"hasPassword"_s, !s.password.isEmpty());
    return map;
}

void MediaMtxServersModel::setPassword(int index, const QString &password)
{
    if (!isValidIndex(index)) {
        return;
    }
    m_servers[index].password = password;
    Q_EMIT dataChanged(this->index(index, 0), this->index(index, 0));
}

bool MediaMtxServersModel::hasPassword(int index) const
{
    return isValidIndex(index) && !m_servers.at(index).password.isEmpty();
}

QString MediaMtxServersModel::password(int index) const
{
    return isValidIndex(index) ? m_servers.at(index).password : QString {};
}

bool MediaMtxServersModel::hasStoredCredential(const QString &serverName, const QString &username) const
{
    QString password;
    return MediaMtxCredentials::findStoredPassword(serverName, username, password);
}

QString MediaMtxServersModel::effectivePassword(int index) const
{
    if (!isValidIndex(index)) {
        return {};
    }
    const Server &s = m_servers.at(index);
    if (!s.password.isEmpty()) {
        return s.password; // a manually entered password takes precedence
    }
    QString stored;
    return MediaMtxCredentials::findStoredPassword(s.name, s.username, stored) ? stored : QString {};
}

bool MediaMtxServersModel::hasUsablePassword(int index) const
{
    return !effectivePassword(index).isEmpty();
}

QString MediaMtxServersModel::apiBaseUrl(int index) const
{
    if (!isValidIndex(index)) {
        return {};
    }
    const Server &s = m_servers.at(index);
    return s.apiScheme + u"://"_s + s.host + u":"_s + QString::number(s.apiPort);
}

QString MediaMtxServersModel::username(int index) const
{
    return isValidIndex(index) ? m_servers.at(index).username : QString {};
}

QString MediaMtxServersModel::name(int index) const
{
    return isValidIndex(index) ? m_servers.at(index).name : QString {};
}

void MediaMtxServersModel::applyDetectedWebRtc(int index, int port, const QString &scheme)
{
    if (!isValidIndex(index)) {
        return;
    }
    Server &s = m_servers[index];
    if (port > 0) {
        s.webRtcPort = port;
    }
    if (!scheme.isEmpty()) {
        s.webRtcScheme = scheme;
    }
    Q_EMIT dataChanged(this->index(index, 0), this->index(index, 0));
}

void MediaMtxServersModel::saveServersToFile()
{
    QJsonArray arr;
    for (const Server &s : std::as_const(m_servers)) {
        QJsonObject o;
        o.insert(u"name"_s, s.name);
        o.insert(u"host"_s, s.host);
        o.insert(u"apiPort"_s, s.apiPort);
        o.insert(u"apiScheme"_s, s.apiScheme);
        o.insert(u"username"_s, s.username);
        o.insert(u"webRtcPort"_s, s.webRtcPort);
        o.insert(u"webRtcScheme"_s, s.webRtcScheme);
        o.insert(u"autoDetectWebRtc"_s, s.autoDetectWebRtc);
        o.insert(u"enabled"_s, s.enabled);
        arr.append(o);
    }

    QJsonObject root;
    root.insert(u"servers"_s, arr);

    QFile serversFile(serversFilePath());
    if (!serversFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("Couldn't write mediamtx-servers file.");
        return;
    }
    serversFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

// ---------------------------------------------------------------------------
// MediaMtxWorker
// ---------------------------------------------------------------------------

MediaMtxWorker::MediaMtxWorker(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

void MediaMtxWorker::doFetch(const QString &baseUrl, const QString &username, const QString &password,
                             bool includeConfiguredPaths, bool fetchGlobalConfig)
{
    m_username = username;
    m_password = password;

    int statusCode = 0;
    QString error;
    QJsonArray pathItems;
    QJsonArray configPathItems;
    QString globalConfigJson;

    if (baseUrl.isEmpty()) {
        Q_EMIT fetchFinished(0, QString {}, QString {}, QString {}, u"No MediaMTX server selected"_s);
        return;
    }

    const QUrl base(baseUrl);
    if (!base.isValid() || base.host().isEmpty()) {
        Q_EMIT fetchFinished(0, QString {}, QString {}, QString {}, u"Invalid MediaMTX API URL: %1"_s.arg(baseUrl));
        return;
    }

    auto fetchList = [this, &statusCode, &error](const QUrl &base, const QString &endpoint,
                                                 QJsonArray &outItems) -> bool {
        // MediaMTX's v3 list endpoints paginate from zero: page=0 is the first page, and a
        // request for page==pageCount answers HTTP 200 with an empty item list. Starting at 1
        // would therefore silently skip every stream on a single-page server (the common case).
        int page = 0;
        int pageCount = 1;
        while (page < pageCount && page <= 100) { // the cap is a safety stop
            QUrl url(base);
            url.setPath(endpoint);
            url.setQuery(QUrlQuery({
                { u"itemsPerPage"_s, u"100"_s },
                { u"page"_s, QString::number(page) },
            }));

            const Response response = get(url);
            if (!response.error.isEmpty()) {
                error = response.error;
                return false;
            }

            statusCode = response.status;
            if (statusCode < 200 || statusCode >= 300) {
                error = u"HTTP %1: %2"_s.arg(statusCode).arg(QString::fromUtf8(response.body).left(200));
                return false;
            }

            const QJsonDocument doc = QJsonDocument::fromJson(response.body);
            if (!doc.isObject()) {
                error = u"Unexpected response from %1"_s.arg(endpoint);
                return false;
            }

            const QJsonObject o = doc.object();
            for (const QJsonValue &value : o.value(u"items"_s).toArray()) {
                outItems.append(value);
            }
            pageCount = o.value(u"pageCount"_s).toInt(1);
            ++page;
        }
        return true;
    };

    if (!fetchList(base, u"/v3/paths/list"_s, pathItems)) {
        Q_EMIT fetchFinished(statusCode, QString {}, QString {}, QString {}, error);
        return;
    }

    if (includeConfiguredPaths) {
        // Configured-but-idle paths are a convenience, failures here are not fatal.
        const QString savedError = error;
        if (!fetchList(base, u"/v3/config/paths/list"_s, configPathItems)) {
            configPathItems = QJsonArray {};
            error = savedError;
        }
    }

    if (fetchGlobalConfig) {
        const Response response = get(QUrl(base.toString() + u"/v3/config/global/get"_s));
        if (response.error.isEmpty() && response.status >= 200 && response.status < 300) {
            globalConfigJson = QString::fromUtf8(response.body);
        }
    }

    Q_EMIT fetchFinished(statusCode,
                         QString::fromUtf8(QJsonDocument(pathItems).toJson(QJsonDocument::Compact)),
                         QString::fromUtf8(QJsonDocument(configPathItems).toJson(QJsonDocument::Compact)),
                         globalConfigJson,
                         QString {});
}

MediaMtxWorker::Response MediaMtxWorker::get(const QUrl &url) const
{
    Response response;
    if (m_network == nullptr) {
        response.error = u"Network access is not available."_s;
        return response;
    }

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    if (!m_username.isEmpty()) {
        // Same approach as WhepClient::applyAuth(): the credentials travel in an
        // Authorization header rather than being embedded in the URL.
        const QByteArray token = (m_username + u":"_s + m_password).toUtf8().toBase64();
        request.setRawHeader("Authorization", "Basic " + token);
    }

    // The worker thread is dedicated to these fetches, so a local event loop per request keeps
    // the flow synchronous without touching the UI thread.
    bool finished = false;
    QEventLoop loop;
    QNetworkReply *reply = m_network->get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, [&finished, &loop] {
        finished = true;
        loop.quit();
    });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(8000);
    loop.exec();
    timeout.stop();

    if (!finished) {
        reply->abort();
        response.error = u"Timed out contacting %1"_s.arg(url.host());
        reply->deleteLater();
        return response;
    }

    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        // Transport-level failure (connection refused, DNS, TLS...); HTTP errors are not.
        response.error = u"Could not connect to %1: %2"_s.arg(url.host(), reply->errorString());
    } else {
        response.body = reply->readAll();
    }

    reply->deleteLater();
    return response;
}

// ---------------------------------------------------------------------------
// MediaMtxModel
// ---------------------------------------------------------------------------

MediaMtxModel::MediaMtxModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_worker = new MediaMtxWorker();
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &MediaMtxModel::startFetch, m_worker, &MediaMtxWorker::doFetch);
    connect(m_worker, &MediaMtxWorker::fetchFinished, this, &MediaMtxModel::onFetchFinished);
    m_workerThread.start();
}

MediaMtxModel::~MediaMtxModel()
{
    m_workerThread.quit();
    m_workerThread.wait();
}

void MediaMtxModel::setServersModel(MediaMtxServersModel *servers)
{
    m_servers = servers;
}

int MediaMtxModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return int(m_streams.size());
}

QVariant MediaMtxModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !checkIndex(index)) {
        return {};
    }

    const Stream &s = m_streams.at(index.row());
    switch (role) {
    case NameRole:
        return s.name;
    case ServerNameRole:
        return s.serverName;
    case WhepUrlRole:
        return s.whepUrl;
    case OnlineRole:
        return s.online;
    case SourceTypeRole:
        return s.sourceType;
    case TracksRole:
        return s.tracks;
    case ReadersRole:
        return s.readers;
    case ConfiguredOnlyRole:
        return s.configuredOnly;
    case RequiresAuthRole:
        return s.requiresAuth;
    default:
        return {};
    }
}

QHash<int, QByteArray> MediaMtxModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { ServerNameRole, "serverName" },
        { WhepUrlRole, "whepUrl" },
        { OnlineRole, "online" },
        { SourceTypeRole, "sourceType" },
        { TracksRole, "tracks" },
        { ReadersRole, "readers" },
        { ConfiguredOnlyRole, "configuredOnly" },
        { RequiresAuthRole, "requiresAuth" },
    };
}

int MediaMtxModel::getNumberOfStreams() const
{
    return int(m_streams.size());
}

int MediaMtxModel::currentServerIndex() const
{
    return m_currentServerIndex;
}

bool MediaMtxModel::refreshInProgress() const
{
    return m_refreshInProgress;
}

QString MediaMtxModel::lastError() const
{
    return m_lastError;
}

int MediaMtxModel::lastStatusCode() const
{
    return m_lastStatusCode;
}

QString MediaMtxModel::lastSummary() const
{
    return m_lastSummary;
}

void MediaMtxModel::clear()
{
    beginResetModel();
    m_streams.clear();
    endResetModel();
    Q_EMIT streamsListChanged();
}

void MediaMtxModel::refresh(int serverIndex)
{
    if (!m_servers || m_refreshInProgress) {
        return;
    }

    const QString baseUrl = m_servers->apiBaseUrl(serverIndex);
    if (baseUrl.isEmpty()) {
        m_lastError = u"No MediaMTX server selected"_s;
        m_lastStatusCode = 0;
        m_lastSummary.clear();
        Q_EMIT responseChanged();
        return;
    }

    m_pendingServerIndex = serverIndex;
    m_testOnly = false;
    m_refreshInProgress = true;
    Q_EMIT refreshInProgressChanged();

    Q_EMIT startFetch(baseUrl, m_servers->username(serverIndex), m_servers->effectivePassword(serverIndex), true, true);
}

void MediaMtxModel::testConnection(int serverIndex)
{
    if (!m_servers || m_refreshInProgress) {
        return;
    }

    const QString baseUrl = m_servers->apiBaseUrl(serverIndex);
    if (baseUrl.isEmpty()) {
        // Same visible feedback as refresh(): a silent return would leave the user with no
        // indication that Test Connection did nothing.
        m_lastError = u"No MediaMTX server selected"_s;
        m_lastStatusCode = 0;
        m_lastSummary.clear();
        Q_EMIT responseChanged();
        return;
    }

    m_pendingServerIndex = serverIndex;
    m_testOnly = true;
    m_refreshInProgress = true;
    Q_EMIT refreshInProgressChanged();

    Q_EMIT startFetch(baseUrl, m_servers->username(serverIndex), m_servers->effectivePassword(serverIndex), false, true);
}

void MediaMtxModel::onFetchFinished(int statusCode, const QString &pathsJson, const QString &configPathsJson,
                                    const QString &globalConfigJson, const QString &error)
{
    m_refreshInProgress = false;
    Q_EMIT refreshInProgressChanged();

    m_lastStatusCode = statusCode;
    m_lastError = error;

    const int serverIndex = m_pendingServerIndex;
    m_pendingServerIndex = -1;

    if (!error.isEmpty()) {
        m_lastSummary.clear();
        Q_EMIT responseChanged();
        return;
    }

    m_currentServerIndex = serverIndex;
    applyGlobalConfig(globalConfigJson);

    if (m_testOnly) {
        m_testOnly = false;
        m_lastSummary = u"Connected"_s;
        Q_EMIT responseChanged();
        return;
    }

    const QString serverName = m_servers ? m_servers->name(serverIndex) : QString {};

    // Paths can be protected with "readAuthentication: internal"; remember which, so adding one
    // to the configuration appends credentials automatically instead of creating an entry that
    // could never connect. The active-paths endpoint does not report this, only the configured
    // paths list does.
    const QJsonArray configItems = QJsonDocument::fromJson(configPathsJson.toUtf8()).array();
    QHash<QString, bool> readAuthRequired;
    for (const QJsonValue &value : configItems) {
        const QJsonObject o = value.toObject();
        const QString name = o.value(u"name"_s).toString();
        if (!name.isEmpty()) {
            readAuthRequired.insert(name, o.value(u"readAuthentication"_s).toString() == u"internal"_s);
        }
    }

    beginResetModel();
    m_streams.clear();

    const QJsonArray items = QJsonDocument::fromJson(pathsJson.toUtf8()).array();
    for (const QJsonValue &value : items) {
        const QJsonObject o = value.toObject();
        Stream s;
        s.name = o.value(u"name"_s).toString();
        if (s.name.isEmpty()) {
            continue;
        }
        s.serverName = serverName;
        s.online = o.value(u"online"_s).toBool(o.value(u"ready"_s).toBool());
        s.sourceType = o.value(u"source"_s).toObject().value(u"type"_s).toString();
        s.readers = int(o.value(u"readers"_s).toArray().size());

        QStringList codecs;
        for (const QJsonValue &track : o.value(u"tracks2"_s).toArray()) {
            codecs.append(track.toObject().value(u"codec"_s).toString());
        }
        if (codecs.isEmpty()) {
            for (const QJsonValue &track : o.value(u"tracks"_s).toArray()) {
                codecs.append(track.toString());
            }
        }
        codecs.removeAll(QString {});
        s.tracks = codecs.join(u", "_s);

        s.whepUrl = buildWhepUrl(s.name, false);
        s.requiresAuth = readAuthRequired.value(s.name, false);
        m_streams.append(s);
    }

    // Add configured paths that are currently not active, so they can still be prepared.
    QStringList activeNames;
    for (const Stream &s : std::as_const(m_streams)) {
        activeNames.append(s.name);
    }

    for (const QJsonValue &value : configItems) {
        const QJsonObject o = value.toObject();
        const QString name = o.value(u"name"_s).toString();
        // Regex/wildcard path configurations cannot be turned into a concrete URL.
        if (name.isEmpty() || activeNames.contains(name) || name.startsWith(QLatin1Char('~'))
                || name == u"all"_s || name == u"all_others"_s) {
            continue;
        }
        Stream s;
        s.name = name;
        s.serverName = serverName;
        s.online = false;
        s.configuredOnly = true;
        s.sourceType = o.value(u"source"_s).toString();
        s.whepUrl = buildWhepUrl(name, false);
        s.requiresAuth = readAuthRequired.value(name, false);
        m_streams.append(s);
    }

    endResetModel();

    m_lastSummary = u"%1 stream(s) found"_s.arg(m_streams.size());
    Q_EMIT streamsListChanged();
    Q_EMIT responseChanged();
}

void MediaMtxModel::applyGlobalConfig(const QString &globalConfigJson)
{
    if (globalConfigJson.isEmpty() || !m_servers || m_currentServerIndex < 0) {
        return;
    }

    const QVariantMap server = m_servers->serverAt(m_currentServerIndex);
    if (!server.value(u"autoDetectWebRtc"_s, true).toBool()) {
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(globalConfigJson.toUtf8());
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject o = doc.object();

    // MediaMTX reports webrtcEncryption as a boolean in current versions and as the string
    // "strict" in older ones; accept both.
    const QJsonValue encryption = o.value(u"webrtcEncryption"_s);
    const bool encrypted = encryption.isBool() ? encryption.toBool() : (encryption.toString() == u"strict"_s);

    m_servers->applyDetectedWebRtc(m_currentServerIndex,
                                   portFromAddress(o.value(u"webrtcAddress"_s).toString(), 8889),
                                   encrypted ? u"https"_s : u"http"_s);
}

QString MediaMtxModel::buildWhepUrl(const QString &pathName, bool includeCredentials) const
{
    if (!m_servers || m_currentServerIndex < 0) {
        return {};
    }

    const QVariantMap server = m_servers->serverAt(m_currentServerIndex);
    QString credentials;
    if (includeCredentials) {
        const QString user = server.value(u"username"_s).toString();
        if (!user.isEmpty()) {
            // WhepClient lifts embedded credentials out of the URL and sends them in an
            // Authorization header, so this never reaches the wire in the request line.
            const QString pass = m_servers->effectivePassword(m_currentServerIndex);
            credentials = percentEncode(user);
            if (!pass.isEmpty()) {
                credentials += u":"_s + percentEncode(pass);
            }
            credentials += u"@"_s;
        }
    }

    return server.value(u"webRtcScheme"_s).toString()
           + u"://"_s
           + credentials
           + server.value(u"host"_s).toString()
           + u":"_s
           + QString::number(server.value(u"webRtcPort"_s).toInt())
           + u"/"_s
           + pathName
           + u"/whep"_s;
}

QString MediaMtxModel::whepUrlAt(int index, bool includeCredentials) const
{
    if (index < 0 || index >= m_streams.size()) {
        return {};
    }
    if (!includeCredentials) {
        return m_streams.at(index).whepUrl;
    }
    return buildWhepUrl(m_streams.at(index).name, true);
}

QString MediaMtxModel::nameAt(int index) const
{
    if (index < 0 || index >= m_streams.size()) {
        return {};
    }
    return m_streams.at(index).name;
}

bool MediaMtxModel::requiresAuthAt(int index) const
{
    if (index < 0 || index >= m_streams.size()) {
        return false;
    }
    return m_streams.at(index).requiresAuth;
}

} // namespace CBridge