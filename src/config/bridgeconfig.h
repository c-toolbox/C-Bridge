/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "core/bridgetypes.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

class QJsonObject;

namespace CBridge {

/// MPEG-TS over UDP multicast. Carries the source bitstream untouched, so no
/// decode or encode happens on this path.
struct TsMulticastSinkConfig {
    QString groupAddress = QStringLiteral("239.1.1.1");
    quint16 port = 5000;
    int ttl = 8;

    /// Local interface address to transmit from. Empty lets the OS pick, which on a
    /// multi-NIC host frequently selects the wrong one.
    QString localAddress;

    int packetSize = 1316; // 7 x 188-byte TS packets, fits a 1500-byte MTU
    int patPeriodMs = 100; // low so receivers can join quickly
    int pcrPeriodMs = 20;

    QJsonObject toJson() const;
    static TsMulticastSinkConfig fromJson(const QJsonObject &json);

    QString url() const;
};

/// Raw RTP over UDP multicast: the H.264/H.265 video and the Opus audio are sent as
/// separate RTP streams, one FFmpeg rtp muxer context per stream. Like the TS path
/// this is a pure passthrough - no decode or encode happens on it. Video goes to
/// `port` and audio to `port + 1`; RTCP sender reports ride on the same sockets.
struct RtpMulticastSinkConfig {
    QString groupAddress = QStringLiteral("239.1.1.1");
    quint16 port = 5004; // video RTP port; audio uses port + 1
    int ttl = 8;

    /// Local interface address to transmit from. Empty lets the OS pick, which on a
    /// multi-NIC host frequently selects the wrong one.
    QString localAddress;

    int packetSize = 1316; // keeps each RTP datagram inside a standard MTU

    QJsonObject toJson() const;
    static RtpMulticastSinkConfig fromJson(const QJsonObject &json);

    /// The udp:// destination of the video stream, for pasting into a player.
    QString videoUrl() const;
    /// The udp:// destination of the audio stream (video port + 1).
    QString audioUrl() const;
};

/// RTSP over RTP/UDP unicast. C-Bridge runs a small RTSP server and serves the
/// source bitstream as MPEG-TS over RTP to each connected player, so like the
/// multicast path no decode or encode happens on this one.
struct RtspSinkConfig {
    int port = 8554; // TCP control port for RTSP signaling

    /// Path players connect with: rtsp://<host>:<port>/<path>.
    QString path = QStringLiteral("stream");

    /// Local interface address to bind and transmit from. Empty lets the OS pick,
    /// which on a multi-NIC host frequently selects the wrong one.
    QString localAddress;

    QJsonObject toJson() const;
    static RtspSinkConfig fromJson(const QJsonObject &json);

    /// The address players connect with, e.g. rtsp://10.0.0.5:8554/stream.
    QString url() const;
};

struct NdiSinkConfig {
    QString senderName;
    QString groups;

    int targetWidth = 0;  // 0 keeps the source size
    int targetHeight = 0;
    int fpsNum = 0;       // 0 keeps the source rate
    int fpsDen = 1;

    bool audioEnabled = true;

    QJsonObject toJson() const;
    static NdiSinkConfig fromJson(const QJsonObject &json);
};

struct SinkConfig {
    SinkKind kind = SinkKind::TsMulticast;
    bool enabled = true;
    TsMulticastSinkConfig ts;
    RtpMulticastSinkConfig rtp;
    RtspSinkConfig rtsp;
    NdiSinkConfig ndi;

    QJsonObject toJson() const;
    static SinkConfig fromJson(const QJsonObject &json);

    QString describe() const;
};

struct StreamConfig {
    QString id;
    QString name;
    bool enabled = true;

    QUrl whepUrl;
    QString username;

    /// Order matters: the first entry is offered with the highest priority.
    QList<VideoCodec> preferredCodecs { VideoCodec::H264, VideoCodec::H265 };
    bool audioEnabled = true;

    int reconnectInitialMs = 500;
    int reconnectMaxMs = 15000;

    QList<SinkConfig> sinks;

    QJsonObject toJson() const;
    static StreamConfig fromJson(const QJsonObject &json);

    static StreamConfig createDefault();
    bool hasSinkOfKind(SinkKind kind) const;
};

/// A saved set of streams. Loaded from disk at startup and edited in the UI.
class BridgeConfig
{
public:
    static constexpr int kSchemaVersion = 1;

    QString name = QStringLiteral("Untitled");
    QList<StreamConfig> streams;

    QJsonObject toJson() const;
    static std::optional<BridgeConfig> fromJson(const QJsonObject &json, QString *error);

    bool save(const QString &path, QString *error) const;
    static std::optional<BridgeConfig> load(const QString &path, QString *error);

    /// Returns human-readable problems that would break a run: duplicate multicast
    /// endpoints, duplicate NDI names, malformed addresses, streams without sinks.
    QStringList validate() const;

    /// The same rules for a single stream, ignoring excludeId so an edited stream
    /// never conflicts with the copy still stored here.
    QStringList validateStream(const StreamConfig &candidate, const QString &excludeId) const;

    int indexOfStream(const QString &id) const;

    /// Picks a multicast endpoint not already used by this config. With audioPort set,
    /// both the port and the one after it must be free (RTP sinks use two ports).
    TsMulticastSinkConfig suggestMulticastEndpoint(bool audioPort = false) const;
};

} // namespace CBridge
