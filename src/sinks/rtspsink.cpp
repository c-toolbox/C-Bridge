/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sinks/rtspsink.h"

#include <QMetaObject>
#include <QUuid>
#include <QThread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

constexpr int kRtpVideoClock = 90000; // also the MPEG-TS timebase
constexpr int kRtpAudioClock = 48000; // Opus
constexpr qint64 kRtpWrap = 1LL << 32;

QString avError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

AVCodecID toAvCodecId(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264: return AV_CODEC_ID_H264;
    case VideoCodec::H265: return AV_CODEC_ID_HEVC;
    case VideoCodec::Unknown: break;
    }
    return AV_CODEC_ID_NONE;
}

/// Pulls one "key=value" parameter out of an RTSP Transport header value.
QString transportParameter(const QString &transport, const char *key)
{
    for (const QString &part : transport.split(u';')) {
        const int eq = part.indexOf(u'=');
        if (eq < 0 || part.left(eq).trimmed() != QLatin1String(key)) {
            continue;
        }
        return part.mid(eq + 1).trimmed();
    }
    return {};
}

/// The first port of a "6000-6001" or plain "6000" client_port value.
int firstPort(const QString &value)
{
    const int dash = value.indexOf(u'-');
    const QString head = dash >= 0 ? value.left(dash) : value;
    bool ok = false;
    const int port = head.trimmed().toInt(&ok);
    return (ok && port > 0 && port <= 65535) ? port : 0;
}

/// The case-insensitive value of one RTSP request header.
QString findHeader(const QList<QByteArray> &lines, const char *name)
{
    for (const QByteArray &line : lines) {
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        if (line.left(colon).compare(name, Qt::CaseInsensitive) == 0) {
            return QString::fromUtf8(line.mid(colon + 1)).trimmed();
        }
    }
    return {};
}

/// Formats an address for a udp:// URL, bracketing IPv6 the way URLs require.
QString urlHost(const QHostAddress &address)
{
    const QString text = address.toString();
    if (address.protocol() == QAbstractSocket::IPv6Protocol && !text.contains(u'[')) {
        return u"[%1]"_s.arg(text);
    }
    return text;
}

/// Converts an IPv4-mapped IPv6 address (::ffff:a.b.c.d) to plain IPv4. Qt reports
/// peers of a dual-stack listener in the mapped form, and sending from an AF_INET6
/// socket to such a destination fails on Windows with WSAEADDRNOTAVAIL; a plain
/// IPv4 URL keeps FFmpeg's udp protocol on the IPv4 family where it works.
QHostAddress normalizeAddress(const QHostAddress &address)
{
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        bool ok = false;
        const quint32 v4 = address.toIPv4Address(&ok);
        if (ok && v4 != 0) {
            return QHostAddress(v4);
        }
    }
    return address;
}

} // namespace

RtspSink::RtspSink(RtspSinkConfig config, QString streamName)
    : m_config(std::move(config))
    , m_streamName(std::move(streamName))
{
}

RtspSink::~RtspSink()
{
    close();
}

QString RtspSink::describe() const
{
    const QString host = m_config.localAddress.isEmpty() ? u"*"_s : m_config.localAddress;
    return u"RTSP %1:%2/%3"_s.arg(host).arg(m_config.port).arg(m_config.path);
}

bool RtspSink::open(const StreamFormat &format, QString *error)
{
    close();

    if (format.videoCodec == VideoCodec::Unknown) {
        if (error) {
            *error = u"Cannot open the RTSP sink without a known video codec"_s;
        }
        return false;
    }

    m_format = format;

    // Bind signaling to the configured interface so it matches where media is sent from.
    QHostAddress bind = QHostAddress::Any;
    if (!m_config.localAddress.isEmpty()) {
        if (!bind.setAddress(m_config.localAddress)) {
            if (error) {
                *error = u"Invalid RTSP local address %1"_s.arg(m_config.localAddress);
            }
            return false;
        }
        bind = normalizeAddress(bind);
    }

    // The control plane runs in its own event loop so SETUP/PLAY never block media.
    QThread *thread = new QThread();
    auto *server = new RtspServer(this);
    server->moveToThread(thread);
    thread->start();

    const int port = m_config.port;
    QString listenError;
    QMetaObject::invokeMethod(server, [server, port, bind, &listenError] {
        listenError = server->start(port, bind);
    }, Qt::BlockingQueuedConnection);

    if (!listenError.isEmpty()) {
        if (error) {
            *error = listenError;
        }
        // QThread::quit() is thread-safe: it posts a quit event to the target loop.
        // Do not queue it through invokeMethod - the QThread object lives in this
        // thread, so the call would be posted to our own (blocked) event loop.
        thread->quit();
        thread->wait();
        delete server;
        delete thread;
        return false;
    }

    m_serverThread = thread;
    m_server = server;
    m_open = true;
    qInfo("RTSP sink %s: serving on port %d", qUtf8Printable(describe()), m_config.port);
    return true;
}

void RtspSink::close()
{
    m_open = false;

    if (m_serverThread) {
        QThread *thread = m_serverThread;
        QObject *server = m_server;
        m_serverThread = nullptr;
        m_server = nullptr;

        // QThread::quit() is thread-safe: it posts a quit event to the target loop.
        // Do not queue it through invokeMethod - the QThread object lives in this
        // thread, so the call would be posted to our own (blocked) event loop and
        // wait() below would deadlock.
        thread->quit();
        thread->wait();
        delete server;
        delete thread;
    }

    {
        std::lock_guard lock(m_clientMutex);
        for (auto &client : m_clients) {
            closeClientContext(client.get());
        }
        m_clients.clear();
    }

    m_videoLastRaw = -1;
    m_audioLastRaw = -1;
    m_videoOffset = 0;
    m_audioOffset = 0;
}

void RtspSink::addClient(const QString &sessionId, const QHostAddress &destination, int rtpPort)
{
    std::lock_guard lock(m_clientMutex);
    // One media stream per session: a second SETUP replaces the first.
    for (auto &client : m_clients) {
        if (client->sessionId == sessionId && !client->dead) {
            client->destination = destination;
            client->rtpPort = rtpPort;
            return;
        }
    }

    auto client = std::make_unique<Client>();
    client->sessionId = sessionId;
    client->destination = destination;
    client->rtpPort = rtpPort;
    m_clients.push_back(std::move(client));
}

void RtspSink::activateClient(const QString &sessionId)
{
    std::lock_guard lock(m_clientMutex);
    for (auto &client : m_clients) {
        if (client->sessionId == sessionId && !client->dead) {
            client->active = true;
        }
    }
}

void RtspSink::killClient(const QString &sessionId)
{
    std::lock_guard lock(m_clientMutex);
    for (auto &client : m_clients) {
        if (client->sessionId == sessionId) {
            client->dead = true;
        }
    }
}

void RtspSink::setDead(Client *client)
{
    std::lock_guard lock(m_clientMutex);
    client->dead = true;
}

void RtspSink::reapDeadClients()
{
    std::lock_guard lock(m_clientMutex);
    m_clients.erase(std::remove_if(m_clients.begin(), m_clients.end(),
        [this](const std::unique_ptr<Client> &client) {
            if (!client->dead) {
                return false;
            }
            closeClientContext(client.get());
            qInfo("RTSP sink %s: client %s:%d left", qUtf8Printable(describe()),
                  client->destination.toString().toUtf8().constData(), client->rtpPort);
            return true;
        }), m_clients.end());
}

void RtspSink::closeClientContext(Client *client)
{
    if (client->packet) {
        av_packet_free(&client->packet);
    }
    if (client->formatCtx) {
        if (client->formatCtx->pb) {
            avio_closep(&client->formatCtx->pb);
        }
        avformat_free_context(client->formatCtx);
    }
    client->formatCtx = nullptr;
    client->videoStream = nullptr;
    client->audioStream = nullptr;
}

bool RtspSink::ensureContext(Client *client)
{
    if (client->formatCtx) {
        return true;
    }

    QString url = u"udp://%1:%2"_s.arg(urlHost(client->destination)).arg(client->rtpPort);
    if (!m_config.localAddress.isEmpty()) {
        url += u"?localaddr=%1"_s.arg(m_config.localAddress);
    }
    const QByteArray utf8 = url.toUtf8();

    AVFormatContext *format = nullptr;
    int ret = avformat_alloc_output_context2(&format, nullptr, "rtp_mpegts", utf8.constData());
    if (ret < 0 || !format) {
        qWarning("RTSP sink %s: could not create the rtp_mpegts muxer for %1:%2: %3",
                 qUtf8Printable(describe()), client->destination.toString().toUtf8().constData(),
                 client->rtpPort, avError(ret).toUtf8().constData());
        return false;
    }

    AVStream *video = avformat_new_stream(format, nullptr);
    if (!video) {
        qWarning("RTSP sink %s: could not allocate the TS video stream",
                 qUtf8Printable(describe()));
        avformat_free_context(format);
        return false;
    }

    AVCodecParameters *parameters = video->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = toAvCodecId(m_format.videoCodec);
    parameters->width = m_format.width;
    parameters->height = m_format.height;
    if (!m_format.videoExtradata.isEmpty()) {
        parameters->extradata = static_cast<uint8_t *>(av_mallocz(
            size_t(m_format.videoExtradata.size()) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (parameters->extradata) {
            std::memcpy(parameters->extradata, m_format.videoExtradata.constData(),
                        size_t(m_format.videoExtradata.size()));
            parameters->extradata_size = int(m_format.videoExtradata.size());
        }
    }
    video->time_base = AVRational { 1, kRtpVideoClock };

    AVStream *audio = nullptr;
    if (m_format.hasAudio) {
        audio = avformat_new_stream(format, nullptr);
        if (!audio) {
            qWarning("RTSP sink %s: could not allocate the TS audio stream",
                     qUtf8Printable(describe()));
            avformat_free_context(format);
            return false;
        }

        AVCodecParameters *audioParams = audio->codecpar;
        audioParams->codec_type = AVMEDIA_TYPE_AUDIO;
        audioParams->codec_id = AV_CODEC_ID_OPUS;
        audioParams->sample_rate = m_format.audioSampleRate;
        av_channel_layout_default(&audioParams->ch_layout, m_format.audioChannels);
        audio->time_base = AVRational { 1, kRtpAudioClock };
    }

    format->flags |= AVFMT_FLAG_FLUSH_PACKETS;

    ret = avio_open2(&format->pb, utf8.constData(), AVIO_FLAG_WRITE, nullptr, nullptr);
    if (ret < 0) {
        qWarning("RTSP sink %s: could not open %1: %2", qUtf8Printable(describe()),
                 url.toUtf8().constData(), avError(ret).toUtf8().constData());
        avformat_free_context(format);
        return false;
    }

    ret = avformat_write_header(format, nullptr);
    if (ret < 0) {
        qWarning("RTSP sink %s: could not write the RTP header for %1:%2: %3",
                 qUtf8Printable(describe()), client->destination.toString().toUtf8().constData(),
                 client->rtpPort, avError(ret).toUtf8().constData());
        if (format->pb) {
            avio_closep(&format->pb);
        }
        avformat_free_context(format);
        return false;
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        qWarning("RTSP sink %s: out of memory allocating an AVPacket",
                 qUtf8Printable(describe()));
        if (format->pb) {
            avio_closep(&format->pb);
        }
        avformat_free_context(format);
        return false;
    }

    client->formatCtx = format;
    client->videoStream = video;
    client->audioStream = audio;
    client->packet = packet;

    qInfo("RTSP sink %s: serving %s:%d", qUtf8Printable(describe()),
          client->destination.toString().toUtf8().constData(), client->rtpPort);
    return true;
}

bool RtspSink::writeToClient(Client *client, AVStream *stream, const std::uint8_t *data,
                             std::size_t size, qint64 pts, bool isKeyframe)
{
    av_packet_unref(client->packet);

    if (av_new_packet(client->packet, int(size)) < 0) {
        return false;
    }
    std::memcpy(client->packet->data, data, size);

    client->packet->stream_index = stream->index;
    client->packet->pts = pts;
    client->packet->dts = pts;
    client->packet->duration = 0;
    if (isKeyframe) {
        client->packet->flags |= AV_PKT_FLAG_KEY;
    }

    const int ret = av_interleaved_write_frame(client->formatCtx, client->packet);
    av_packet_unref(client->packet);

    if (ret < 0) {
        qWarning("RTSP sink %s: write to %s:%d failed: %s", qUtf8Printable(describe()),
                 client->destination.toString().toUtf8().constData(), client->rtpPort,
                 avError(ret).toUtf8().constData());
        return false;
    }

    m_bytesWritten.fetch_add(size, std::memory_order_relaxed);
    return true;
}

qint64 RtspSink::unwrap(std::uint32_t rtpTimestamp, qint64 &lastRaw, qint64 &offset)
{
    const qint64 raw = qint64(rtpTimestamp);

    if (lastRaw >= 0) {
        const qint64 delta = raw - lastRaw;
        if (delta < -(kRtpWrap / 2)) {
            offset += kRtpWrap;
        } else if (delta > (kRtpWrap / 2)) {
            offset -= kRtpWrap;
        }
    }

    lastRaw = raw;
    return raw + offset;
}

bool RtspSink::writeVideo(const std::uint8_t *data, std::size_t size,
                          std::uint32_t rtpTimestamp, bool isKeyframe)
{
    if (!m_open || size == 0) {
        return false;
    }

    reapDeadClients();

    const qint64 absolute = unwrap(rtpTimestamp, m_videoLastRaw, m_videoOffset);

    std::vector<Client *> targets;
    {
        std::lock_guard lock(m_clientMutex);
        for (auto &client : m_clients) {
            Client *candidate = client.get();
            if (!candidate->active || candidate->dead) {
                continue;
            }
            // Hold each client until its first keyframe so it always starts on a clean GOP.
            if (!candidate->started && !isKeyframe) {
                continue;
            }
            targets.push_back(candidate);
        }
    }

    bool anyWritten = false;
    for (Client *client : targets) {
        if (!ensureContext(client)) {
            setDead(client); // the muxer failed; drop this client
            continue;
        }
        if (!client->started) {
            client->ptsBase = absolute;
            client->started = true;
        }

        const qint64 pts = absolute - client->ptsBase;
        if (writeToClient(client, client->videoStream, data, size, pts, isKeyframe)) {
            anyWritten = true;
        } else {
            setDead(client); // the socket failed: the player went away
        }
    }

    return anyWritten || targets.empty();
}

bool RtspSink::writeAudio(const std::uint8_t *data, std::size_t size,
                          std::uint32_t rtpTimestamp)
{
    if (!m_open || !m_format.hasAudio || size == 0) {
        return false;
    }

    reapDeadClients();

    const qint64 absolute = unwrap(rtpTimestamp, m_audioLastRaw, m_audioOffset);

    // ptsBase lives on the 90 kHz video clock while audio arrives at 48 kHz, so
    // rebase in the video domain and express the result in the audio timebase.
    const qint64 videoDomain = av_rescale(absolute, kRtpVideoClock, kRtpAudioClock);

    std::vector<Client *> targets;
    {
        std::lock_guard lock(m_clientMutex);
        for (auto &client : m_clients) {
            Client *candidate = client.get();
            // Audio before the first video frame would land at a negative PTS.
            if (candidate->active && !candidate->dead && candidate->started) {
                targets.push_back(candidate);
            }
        }
    }

    bool anyWritten = false;
    for (Client *client : targets) {
        const qint64 pts = av_rescale(videoDomain - client->ptsBase, kRtpAudioClock, kRtpVideoClock);
        if (writeToClient(client, client->audioStream, data, size, pts, true)) {
            anyWritten = true;
        } else {
            setDead(client);
        }
    }

    return anyWritten || targets.empty();
}

QString RtspSink::sdp() const
{
    // The m= port is filled in by SETUP; players replace it with the transport
    // parameters from the response, exactly like FFmpeg's own rtp_mpegts SDP.
    const QString host = m_config.localAddress.isEmpty() ? u"0.0.0.0"_s : m_config.localAddress;
    return u"v=0\r\n"
           u"o=- 0 0 IN IP4 %1\r\n"
           u"s=C-Bridge - %2\r\n"
           u"c=IN IP4 0.0.0.0\r\n"
           u"t=0 0\r\n"
           u"m=video 0 RTP/AVP 33\r\n"
           u"a=rtpmap:33 MP2T/90000\r\n"_s.arg(host, m_streamName);
}

RtspServer::RtspServer(RtspSink *sink)
    : m_sink(sink)
    , m_tcp(new QTcpServer(this))
{
}

QString RtspServer::start(int port, const QHostAddress &bind)
{
    if (!m_tcp->listen(bind, quint16(port))) {
        return u"Could not listen on RTSP %1:%2: %3"_s.arg(bind.toString()).arg(port).arg(m_tcp->errorString());
    }
    connect(m_tcp, &QTcpServer::newConnection, this, [this] {
        onNewConnection();
    });
    return {};
}

void RtspServer::onNewConnection()
{
    while (QTcpSocket *socket = m_tcp->nextPendingConnection()) {
        auto *connection = new RtspConnection(this, m_sink, socket);
        connect(socket, &QTcpSocket::readyRead, connection, [connection] {
            connection->onReadyRead();
        });
        connect(socket, &QTcpSocket::disconnected, connection, [this, connection] {
            m_sink->killClient(connection->sessionId());
            connection->deleteLater();
        });
    }
}

RtspConnection::RtspConnection(QObject *parent, RtspSink *sink, QTcpSocket *socket)
    : QObject(parent)
    , m_sink(sink)
    , m_socket(socket)
{
    m_socket->setParent(this);
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void RtspConnection::onReadyRead()
{
    m_buffer.append(m_socket->readAll());

    int end;
    while ((end = m_buffer.indexOf("\r\n\r\n")) >= 0) {
        const QByteArray request = m_buffer.left(end);
        m_buffer.remove(0, end + 4);
        handleRequest(request);
    }
}

void RtspConnection::handleRequest(const QByteArray &request)
{
    const QList<QByteArray> lines = request.split('\n');
    if (lines.isEmpty()) {
        return;
    }

    const QList<QByteArray> parts = lines.first().trimmed().split(' ');
    if (parts.size() < 2) {
        respond(0, 400, u"Bad Request"_s);
        return;
    }

    // Serve only our own path so a mistyped URL fails loudly instead of playing.
    const QString url = QString::fromUtf8(parts[1]);
    const int schemeEnd = url.indexOf(u"://");
    if (schemeEnd >= 0) {
        const QString requestedPath = url.mid(schemeEnd + 3).section('/', 1);
        if (!requestedPath.isEmpty() && requestedPath != m_sink->path()) {
            respond(0, 404, u"Not Found"_s);
            return;
        }
    }

    const QByteArray method = parts[0].toUpper();
    const int cseq = findHeader(lines, "CSeq").toInt();

    if (method == "OPTIONS") {
        respond(cseq, 200, u"OK"_s);
    } else if (method == "DESCRIBE") {
        respond(cseq, 200, u"OK"_s, m_sink->sdp().toUtf8(), u"application/sdp"_s);
    } else if (method == "SETUP") {
        const QString transport = findHeader(lines, "Transport");
        // Only unicast UDP is supported; this sink exists for exactly that.
        if (!transport.startsWith(u"RTP/AVP", Qt::CaseInsensitive) ||
            !transport.contains(u"unicast", Qt::CaseInsensitive)) {
            respond(cseq, 461, u"Unsupported Transport"_s);
            return;
        }

        const int clientPort = firstPort(transportParameter(transport, "client_port"));
        if (clientPort == 0) {
            respond(cseq, 461, u"Unsupported Transport"_s);
            return;
        }

        QHostAddress destination(transportParameter(transport, "destination"));
        if (destination.isNull()) {
            destination = m_socket->peerAddress();
        }
        destination = normalizeAddress(destination);

        m_sink->addClient(m_sessionId, destination, clientPort);
        respond(cseq, 200, u"OK"_s, {}, {}, u"Transport: %1\r\n"_s.arg(transport));
    } else if (method == "PLAY") {
        m_sink->activateClient(m_sessionId);
        respond(cseq, 200, u"OK"_s);
    } else if (method == "TEARDOWN") {
        m_sink->killClient(m_sessionId);
        respond(cseq, 200, u"OK"_s);
    } else {
        respond(cseq, 501, u"Not Implemented"_s);
    }
}

void RtspConnection::respond(int cseq, int status, const QString &reason,
                             const QByteArray &body, const QString &contentType,
                             const QString &extraHeaders)
{
    QString response;
    // The status line is mandatory: players reject a response that does not start with it.
    response += u"RTSP/1.0 %1 %2\r\n"_s.arg(status).arg(reason);
    if (status == 200) {
        response += u"Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n";
    }
    response += u"CSeq: %1\r\n"_s.arg(cseq);
    response += u"Session: %1\r\n"_s.arg(m_sessionId);
    if (!extraHeaders.isEmpty()) {
        response += extraHeaders;
    }
    if (!body.isEmpty()) {
        response += u"Content-Type: %1\r\n"_s.arg(contentType);
        response += u"Content-Length: %1\r\n"_s.arg(body.size());
    }
    response += u"\r\n";

    m_socket->write(response.toUtf8() + body);
    m_socket->flush();
}

} // namespace CBridge