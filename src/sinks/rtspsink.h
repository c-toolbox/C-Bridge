/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "sinks/streamsink.h"

#include <QHostAddress>
#include <QObject>
#include <QMutex>
#include <QTcpServer>
#include <QTcpSocket>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class QThread;
struct AVFormatContext;
struct AVPacket;
struct AVStream;

namespace CBridge {

/// Serves a stream over RTSP to individual players, delivering the media as
/// MPEG-TS over RTP/UDP unicast.
///
/// The sink runs its own small RTSP server (OPTIONS, DESCRIBE, SETUP, PLAY and
/// TEARDOWN) on a dedicated thread so signaling never blocks the media path. Each
/// client that reaches PLAY gets one FFmpeg rtp_mpegts context pointed at the port
/// it offered in SETUP, which keeps this a pure passthrough like the multicast
/// sink: no decoder or encoder is involved.
class RtspSink final : public StreamSink
{
public:
    RtspSink(RtspSinkConfig config, QString streamName);
    ~RtspSink() override;

    /// The path this sink serves, e.g. "stream" in rtsp://host:8554/stream.
    const QString &path() const { return m_config.path; }

    QString describe() const override;

    bool open(const StreamFormat &format, QString *error) override;
    void close() override;
    bool isOpen() const override { return m_open; }

    bool writeVideo(const std::uint8_t *data, std::size_t size,
                    std::uint32_t rtpTimestamp, bool isKeyframe) override;
    bool writeAudio(const std::uint8_t *data, std::size_t size,
                    std::uint32_t rtpTimestamp) override;

    quint64 bytesWritten() const override
    {
        return m_bytesWritten.load(std::memory_order_relaxed);
    }

private:
    friend class RtspServer;
    friend class RtspConnection;
    /// One connected player. The metadata is shared between the RTSP server thread
    /// and the pipeline worker; the FFmpeg state below it is touched by the worker
    /// only, so a client's muxer can never be written while it is being torn down.
    struct Client {
        QString sessionId;
        QHostAddress destination;
        int rtpPort = 0;

        AVFormatContext *formatCtx = nullptr;
        AVStream *videoStream = nullptr;
        AVStream *audioStream = nullptr;
        AVPacket *packet = nullptr;

        bool active = false; // PLAY received
        bool dead = false;   // TEARDOWN or disconnect; the worker reaps it
        bool started = false; // first keyframe written, PTS anchored there

        qint64 ptsBase = -1;
    };

    // Called from the RTSP server thread.
    void addClient(const QString &sessionId, const QHostAddress &destination, int rtpPort);
    void activateClient(const QString &sessionId);
    void killClient(const QString &sessionId);

    // Worker thread only.
    void setDead(Client *client);
    void reapDeadClients();
    bool ensureContext(Client *client);
    static void closeClientContext(Client *client);
    bool writeToClient(Client *client, AVStream *stream, const std::uint8_t *data,
                       std::size_t size, qint64 pts, bool isKeyframe);

    /// Converts a wrapping 32-bit RTP timestamp into a monotonic 64-bit PTS.
    static qint64 unwrap(std::uint32_t rtpTimestamp, qint64 &lastRaw, qint64 &offset);

    QString sdp() const;

    RtspSinkConfig m_config;
    QString m_streamName;

    QThread *m_serverThread = nullptr;
    QObject *m_server = nullptr; // the RTSP server object living in m_serverThread

    mutable QMutex m_clientMutex;
    std::vector<std::unique_ptr<Client>> m_clients;

    StreamFormat m_format;

    bool m_open = false;

    qint64 m_videoLastRaw = -1;
    qint64 m_videoOffset = 0;
    qint64 m_audioLastRaw = -1;
    qint64 m_audioOffset = 0;

    std::atomic<quint64> m_bytesWritten { 0 };
};

/// The RTSP control plane: a TCP listener and one handler per connection. It lives
/// in its own event loop so SETUP/PLAY never block the media path, and it touches
/// sink state only through the mutex-guarded client list.
class RtspServer : public QObject
{
    Q_OBJECT

public:
    explicit RtspServer(RtspSink *sink);

    /// Binds the listener; called from the worker thread via a blocking queued call.
    /// Returns an empty string on success.
    QString start(int port, const QHostAddress &bind);

private:
    void onNewConnection();

    RtspSink *m_sink;
    // Child (not value member) so it moves with the server into its worker thread;
    // a value member would stay in the creating thread and trip Qt's cross-thread
    // parenting checks.
    QTcpServer *m_tcp = nullptr;
};

/// One RTSP control connection: parses requests, answers them and reports the
/// client's lifecycle to the sink. Lives in the server thread; media never flows
/// through it.
class RtspConnection : public QObject
{
    Q_OBJECT

public:
    RtspConnection(QObject *parent, RtspSink *sink, QTcpSocket *socket);

    QString sessionId() const { return m_sessionId; }

private:
    friend class RtspServer;

    void onReadyRead();
    void handleRequest(const QByteArray &request);
    void respond(int cseq, int status, const QString &reason,
                 const QByteArray &body = {}, const QString &contentType = {},
                 const QString &extraHeaders = {});

    RtspSink *m_sink;
    QTcpSocket *m_socket; // owned by this object
    QString m_sessionId;
    QByteArray m_buffer;
};

} // namespace CBridge