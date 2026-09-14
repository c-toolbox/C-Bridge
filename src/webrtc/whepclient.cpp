#include "webrtc/whepclient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

constexpr auto kSdpContentType = "application/sdp";

} // namespace

WhepClient::WhepClient(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    m_network->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

WhepClient::~WhepClient()
{
    if (m_pending) {
        m_pending->abort();
    }
}

void WhepClient::setEndpoint(const QUrl &url)
{
    m_endpoint = url;

    // Credentials embedded in the URL are common for MediaMTX; lift them out so
    // they travel in the Authorization header instead of the request line.
    if (!m_endpoint.userName().isEmpty()) {
        m_username = m_endpoint.userName();
        m_password = m_endpoint.password();
        m_endpoint.setUserName(QString());
        m_endpoint.setPassword(QString());
    }
}

void WhepClient::setCredentials(const QString &username, const QString &password)
{
    if (!username.isEmpty()) {
        m_username = username;
        m_password = password;
    }
}

bool WhepClient::hasSession() const
{
    return !m_resourceUrl.isEmpty();
}

void WhepClient::applyAuth(QNetworkRequest &request) const
{
    if (m_username.isEmpty()) {
        return;
    }
    const QByteArray token = (m_username + u":"_s + m_password).toUtf8().toBase64();
    request.setRawHeader("Authorization", "Basic " + token);
}

QList<IceServerSpec> WhepClient::iceServersFrom(QNetworkReply *reply) const
{
    QList<IceServerSpec> servers;
    const auto headers = reply->rawHeaderPairs();
    for (const auto &[name, value] : headers) {
        if (name.compare("link", Qt::CaseInsensitive) == 0) {
            servers.append(parseIceServerLinkHeader(QString::fromUtf8(value)));
        }
    }
    return servers;
}

void WhepClient::requestIceServers()
{
    QNetworkRequest request(m_endpoint);
    applyAuth(request);

    QNetworkReply *reply = m_network->sendCustomRequest(request, "OPTIONS");
    QPointer<WhepClient> self(this);
    connect(reply, &QNetworkReply::finished, this, [this, reply, self] {
        reply->deleteLater();
        if (!self) {
            return;
        }
        Q_EMIT iceServersReady(iceServersFrom(reply));
    });
}

void WhepClient::sendOffer(const QString &sdpOffer)
{
    if (m_pending) {
        m_pending->abort();
        m_pending = nullptr;
    }

    QNetworkRequest request(m_endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(kSdpContentType));
    request.setRawHeader("Accept", kSdpContentType);
    applyAuth(request);

    QNetworkReply *reply = m_network->post(request, sdpOffer.toUtf8());
    m_pending = reply;

    QPointer<WhepClient> self(this);
    connect(reply, &QNetworkReply::finished, this, [this, reply, self] {
        reply->deleteLater();
        if (!self) {
            return;
        }
        if (m_pending == reply) {
            m_pending = nullptr;
        }

        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError && status != 201 && status != 200) {
            Q_EMIT failed(u"WHEP POST failed (HTTP %1): %2"_s.arg(status).arg(reply->errorString()));
            return;
        }
        if (status != 201 && status != 200) {
            Q_EMIT failed(u"WHEP POST returned unexpected status %1"_s.arg(status));
            return;
        }

        const QByteArray location = reply->rawHeader("Location");
        if (!location.isEmpty()) {
            m_resourceUrl = m_endpoint.resolved(QUrl(QString::fromUtf8(location)));
        }

        const QString answer = QString::fromUtf8(reply->readAll());
        if (answer.trimmed().isEmpty()) {
            Q_EMIT failed(u"WHEP POST returned an empty SDP answer"_s);
            return;
        }

        Q_EMIT answerReceived(answer);
    });
}

void WhepClient::deleteSession()
{
    if (m_resourceUrl.isEmpty()) {
        return;
    }

    QNetworkRequest request(m_resourceUrl);
    applyAuth(request);
    m_resourceUrl.clear();

    QNetworkReply *reply = m_network->deleteResource(request);
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

} // namespace CBridge
