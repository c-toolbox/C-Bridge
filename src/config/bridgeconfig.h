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

/// SRT receive endpoint. The stream is expected to carry an MPEG-TS container with
/// H.264/H.265 video and Opus audio, which C-Bridge passes through untouched.
struct SrtSourceConfig {
    enum class Mode {
        Listener, // wait for the encoder to connect (srt://:<port>)
        Caller,   // dial out to an existing listener (srt://<host>:<port>)
    };

    Mode mode = Mode::Listener;
    QString host;      // caller mode only
    quint16 port = 9000;
    QString passphrase; // optional AES-128 encryption, must match the encoder's
    int latencyMs = 120; // receive latency for burst absorption

    /// SRT streamid sent by a caller to the listener it dials out to. MediaMTX expects
    /// "read:<path>" (optionally followed by ":<user>:<pass>") on its SRT port; other
    /// servers may use it for access control or routing. Empty leaves it at the default.
    QString streamId;

    QJsonObject toJson() const;
    static SrtSourceConfig fromJson(const QJsonObject &json);

    /// The srt:// endpoint this source listens on or dials out to. A listener with no
    /// explicit host binds 0.0.0.0: an empty host would resolve to the IPv6 wildcard,
    /// which SRT refuses to bind when SRTO_IPV6ONLY is not set.
    QString url() const;
};

/// YouTube ingest via a yt-dlp child process (see YTDLP_SOURCE_PLAN.md). The video passes
/// through as Annex-B from an HLS (MPEG-TS) format; the AAC audio is transcoded to Opus
/// 48 kHz stereo inside the source, so every sink sees the same passthrough contract as
/// WHEP and SRT.
struct YouTubeSourceConfig {
    QUrl url;

    /// yt-dlp format selector: independent video/audio IDs use "video+audio". FFmpeg
    /// fetches and merges them into a streaming MPEG-TS pipe, converting video as needed.
    QString formatSelector = QStringLiteral("bestvideo+bestaudio/best");

    /// Extra yt-dlp arguments appended verbatim (split on spaces), e.g. cookies or a JS
    /// runtime: "--cookies-from-browser chrome".
    QString extraArgs;

    /// Explicit path to yt-dlp.exe for this stream. Empty falls back to the app-wide
    /// Settings value, then PATH, then D:/FFmpeg/yt-dlp.exe.
    QString ytDlpPath;

    /// Bitrate of the AAC→Opus transcode inside the source (YouTube audio is always
    /// converted to Opus 48 kHz stereo before it reaches the pipeline).
    int audioBitrateKbps = 128;

    /// Parallel HLS fragment fetches in yt-dlp (1 = strictly sequential, the original
    /// behaviour). yt-dlp writes fragments to stdout in index order, so the pipe stays
    /// one ordered stream while the network fetch runs ahead of the consumer.
    int concurrentFragments = 4;

    /// "auto": when the metadata probe finds a muxed H.264+AAC HLS format, skip the
    /// yt-dlp stdout pipe entirely and let FFmpeg's HLS reader fetch the resolved direct
    /// URL (reconnects internally, seeks instantly). "off": always pipe through yt-dlp.
    /// "force": prefer the direct path and warn when the probe found no muxed format.
    /// A direct-path failure always falls back to the pipe, so this is an optimisation,
    /// never a new single point of failure.
    QString directUrlMode = QStringLiteral("auto");

    QJsonObject toJson() const;
    static YouTubeSourceConfig fromJson(const QJsonObject &json);
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

    SourceKind sourceKind = SourceKind::Whep; // default keeps every existing config loading unchanged
    QUrl whepUrl;
    QString username;
    SrtSourceConfig srt;
    YouTubeSourceConfig youtube;

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
