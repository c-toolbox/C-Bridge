/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config/bridgeconfig.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

int clampInt(int value, int lo, int hi)
{
    return std::min(hi, std::max(lo, value));
}

/// QVariant::toInt()/toBool() have no default-value overload, and a QJsonValue converted to
/// a QVariant stays *valid* even when the key is absent (it is Undefined, not invalid), so
/// toInt() would silently yield 0. The presence test must be contains(); only then does a
/// missing key fall back instead of becoming 0/false.
int jsonInt(const QJsonObject &json, const QString &key, int fallback)
{
    return json.contains(key) ? json.value(key).toInt() : fallback;
}

bool jsonBool(const QJsonObject &json, const QString &key, bool fallback)
{
    return json.contains(key) ? json.value(key).toBool() : fallback;
}

QString jsonString(const QJsonObject &json, const QString &key, const QString &fallback)
{
    const QString value = json.value(key).toString();
    return value.isEmpty() ? fallback : value;
}

bool isMulticastV4(const QString &address)
{
    QHostAddress host;
    if (!host.setAddress(address) || host.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }
    const quint32 raw = host.toIPv4Address();
    return (raw >> 28) == 0xE; // 224.0.0.0/4
}

/// RTSP paths are relative and never carry a leading slash.
QString sanitizeRtspPath(const QString &raw)
{
    QString path = raw.trimmed();
    while (path.startsWith(u'/')) {
        path.remove(0, 1);
    }
    return path;
}

} // namespace

QJsonObject TsMulticastSinkConfig::toJson() const
{
    return QJsonObject {
        { u"groupAddress"_s, groupAddress },
        { u"port"_s, int(port) },
        { u"ttl"_s, ttl },
        { u"localAddress"_s, localAddress },
        { u"packetSize"_s, packetSize },
        { u"patPeriodMs"_s, patPeriodMs },
        { u"pcrPeriodMs"_s, pcrPeriodMs },
    };
}

TsMulticastSinkConfig TsMulticastSinkConfig::fromJson(const QJsonObject &json)
{
    TsMulticastSinkConfig config;
    config.groupAddress = jsonString(json, u"groupAddress"_s, config.groupAddress);
    config.port = quint16(clampInt(jsonInt(json, u"port"_s, int(config.port)), 1, 65535));
    config.ttl = clampInt(jsonInt(json, u"ttl"_s, config.ttl), 1, 255);
    config.localAddress = json.value(u"localAddress"_s).toString();
    config.packetSize = clampInt(jsonInt(json, u"packetSize"_s, config.packetSize), 188, 65535);
    config.patPeriodMs = clampInt(jsonInt(json, u"patPeriodMs"_s, config.patPeriodMs), 10, 5000);
    config.pcrPeriodMs = clampInt(jsonInt(json, u"pcrPeriodMs"_s, config.pcrPeriodMs), 10, 500);
    return config;
}

QString TsMulticastSinkConfig::url() const
{
    QString url = u"udp://%1:%2?pkt_size=%3&ttl=%4&overrun_nonfatal=1&fifo_size=5000000"_s
        .arg(groupAddress)
        .arg(port)
        .arg(packetSize)
        .arg(ttl);

    if (!localAddress.isEmpty()) {
        url += u"&localaddr=%1"_s.arg(localAddress);
    }
    return url;
}

QJsonObject RtpMulticastSinkConfig::toJson() const
{
    return QJsonObject {
        { u"groupAddress"_s, groupAddress },
        { u"port"_s, int(port) },
        { u"ttl"_s, ttl },
        { u"localAddress"_s, localAddress },
        { u"packetSize"_s, packetSize },
    };
}

RtpMulticastSinkConfig RtpMulticastSinkConfig::fromJson(const QJsonObject &json)
{
    RtpMulticastSinkConfig config;
    config.groupAddress = jsonString(json, u"groupAddress"_s, config.groupAddress);
    // The audio stream uses port + 1, so the video port may not be the last one.
    config.port = quint16(clampInt(jsonInt(json, u"port"_s, int(config.port)), 1, 65534));
    config.ttl = clampInt(jsonInt(json, u"ttl"_s, config.ttl), 1, 255);
    config.localAddress = json.value(u"localAddress"_s).toString();
    // The RTP header alone is 12 bytes; below ~60 the payload would be useless.
    config.packetSize = clampInt(jsonInt(json, u"packetSize"_s, config.packetSize), 64, 65535);
    return config;
}

QString RtpMulticastSinkConfig::videoUrl() const
{
    return u"udp://%1:%2"_s.arg(groupAddress).arg(port);
}

QString RtpMulticastSinkConfig::audioUrl() const
{
    return u"udp://%1:%2"_s.arg(groupAddress).arg(int(port) + 1);
}

QJsonObject RtspSinkConfig::toJson() const
{
    return QJsonObject {
        { u"port"_s, port },
        { u"path"_s, path },
        { u"localAddress"_s, localAddress },
    };
}

RtspSinkConfig RtspSinkConfig::fromJson(const QJsonObject &json)
{
    RtspSinkConfig config;
    config.port = clampInt(jsonInt(json, u"port"_s, config.port), 1, 65535);
    const QString path = sanitizeRtspPath(jsonString(json, u"path"_s, config.path));
    config.path = path.isEmpty() ? QStringLiteral("stream") : path;
    config.localAddress = json.value(u"localAddress"_s).toString();
    return config;
}

QString RtspSinkConfig::url() const
{
    const QString host = localAddress.isEmpty() ? u"0.0.0.0"_s : localAddress;
    return u"rtsp://%1:%2/%3"_s.arg(host).arg(port).arg(path);
}

QJsonObject NdiSinkConfig::toJson() const
{
    return QJsonObject {
        { u"senderName"_s, senderName },
        { u"groups"_s, groups },
        { u"targetWidth"_s, targetWidth },
        { u"targetHeight"_s, targetHeight },
        { u"fpsNum"_s, fpsNum },
        { u"fpsDen"_s, fpsDen },
        { u"audioEnabled"_s, audioEnabled },
    };
}

NdiSinkConfig NdiSinkConfig::fromJson(const QJsonObject &json)
{
    NdiSinkConfig config;
    config.senderName = json.value(u"senderName"_s).toString();
    config.groups = json.value(u"groups"_s).toString();
    config.targetWidth = std::max(0, json.value(u"targetWidth"_s).toInt());
    config.targetHeight = std::max(0, json.value(u"targetHeight"_s).toInt());
    config.fpsNum = std::max(0, json.value(u"fpsNum"_s).toInt());
    config.fpsDen = std::max(1, jsonInt(json, u"fpsDen"_s, 1));
    config.audioEnabled = jsonBool(json, u"audioEnabled"_s, true);
    return config;
}

QJsonObject SrtSourceConfig::toJson() const
{
    return QJsonObject {
        { u"mode"_s, mode == Mode::Listener ? u"listener"_s : u"caller"_s },
        { u"host"_s, host },
        { u"port"_s, int(port) },
        { u"passphrase"_s, passphrase },
        { u"latencyMs"_s, latencyMs },
        { u"streamId"_s, streamId },
    };
}

SrtSourceConfig SrtSourceConfig::fromJson(const QJsonObject &json)
{
    SrtSourceConfig config;
    const QString mode = jsonString(json, u"mode"_s, u"listener"_s);
    config.mode = (mode == u"caller"_s) ? Mode::Caller : Mode::Listener;
    config.host = json.value(u"host"_s).toString();
    config.port = quint16(clampInt(jsonInt(json, u"port"_s, 9000), 1, 65535));
    config.passphrase = json.value(u"passphrase"_s).toString();
    config.latencyMs = clampInt(jsonInt(json, u"latencyMs"_s, 120), 0, 60000);
    config.streamId = json.value(u"streamId"_s).toString();
    return config;
}

QString SrtSourceConfig::url() const
{
    // A listener with no explicit host binds every IPv4 interface. The host part must not be
    // empty: it would resolve to the IPv6 wildcard, which SRT refuses to bind (SRT_EINVOP)
    // because FFmpeg does not set SRTO_IPV6ONLY on the socket.
    if (mode == Mode::Listener && host.trimmed().isEmpty()) {
        return u"srt://0.0.0.0:%1"_s.arg(port);
    }
    return u"srt://%1:%2"_s.arg(host).arg(port);
}

QJsonObject YouTubeSourceConfig::toJson() const
{
    return QJsonObject {
        { u"url"_s, url.toString() },
        { u"formatSelector"_s, formatSelector },
        { u"extraArgs"_s, extraArgs },
        { u"ytDlpPath"_s, ytDlpPath },
        { u"audioBitrateKbps"_s, audioBitrateKbps },
        { u"concurrentFragments"_s, concurrentFragments },
        { u"directUrlMode"_s, directUrlMode },
    };
}

YouTubeSourceConfig YouTubeSourceConfig::fromJson(const QJsonObject &json)
{
    YouTubeSourceConfig config;
    config.url = QUrl(json.value(u"url"_s).toString());
    // An empty selector is meaningful ("let yt-dlp choose"), so only a missing key keeps the
    // built-in default.
    if (json.contains(u"formatSelector"_s)) {
        config.formatSelector = json.value(u"formatSelector"_s).toString();
    }
    config.extraArgs = json.value(u"extraArgs"_s).toString();
    config.ytDlpPath = json.value(u"ytDlpPath"_s).toString();
    // Opus encodes between 6 and 510 kbps; the practical range for speech/music feeds.
    config.audioBitrateKbps = clampInt(jsonInt(json, u"audioBitrateKbps"_s, 128), 32, 510);
    config.concurrentFragments = clampInt(jsonInt(json, u"concurrentFragments"_s, 4), 1, 16);
    const QString mode = json.value(u"directUrlMode"_s).toString().trimmed().toLower();
    config.directUrlMode = (mode == u"off"_s || mode == u"force"_s) ? mode : u"auto"_s;
    return config;
}

QJsonObject SinkConfig::toJson() const
{
    QJsonObject json {
        { u"kind"_s, CBridge::toString(kind) },
        { u"enabled"_s, enabled },
    };

    switch (kind) {
    case SinkKind::TsMulticast:
        json.insert(u"tsMulticast"_s, ts.toJson());
        break;
    case SinkKind::RtpMulticast:
        json.insert(u"rtpMulticast"_s, rtp.toJson());
        break;
    case SinkKind::RtspUnicast:
        json.insert(u"rtsp"_s, rtsp.toJson());
        break;
    case SinkKind::Ndi:
        json.insert(u"ndi"_s, ndi.toJson());
        break;
    }
    return json;
}

SinkConfig SinkConfig::fromJson(const QJsonObject &json)
{
    SinkConfig config;
    config.kind = sinkKindFromString(json.value(u"kind"_s).toString());
    config.enabled = jsonBool(json, u"enabled"_s, true);
    config.ts = TsMulticastSinkConfig::fromJson(json.value(u"tsMulticast"_s).toObject());
    config.rtp = RtpMulticastSinkConfig::fromJson(json.value(u"rtpMulticast"_s).toObject());
    config.rtsp = RtspSinkConfig::fromJson(json.value(u"rtsp"_s).toObject());
    config.ndi = NdiSinkConfig::fromJson(json.value(u"ndi"_s).toObject());
    return config;
}

QString SinkConfig::describe() const
{
    switch (kind) {
    case SinkKind::TsMulticast:
        return u"TS %1:%2"_s.arg(ts.groupAddress).arg(ts.port);
    case SinkKind::RtpMulticast:
        // Video port and the audio port right after it.
        return u"RTP %1:%2/%3"_s.arg(rtp.groupAddress).arg(int(rtp.port)).arg(int(rtp.port) + 1);
    case SinkKind::RtspUnicast: {
        const QString host = rtsp.localAddress.isEmpty() ? u"*"_s : rtsp.localAddress;
        return u"RTSP %1:%2/%3"_s.arg(host).arg(rtsp.port).arg(rtsp.path);
    }
    case SinkKind::Ndi:
        return u"NDI %1"_s.arg(ndi.senderName);
    }
    return {};
}

QJsonObject StreamConfig::toJson() const
{
    QJsonArray codecs;
    for (VideoCodec codec : preferredCodecs) {
        codecs.append(CBridge::toString(codec));
    }

    QJsonArray sinkArray;
    for (const SinkConfig &sink : sinks) {
        sinkArray.append(sink.toJson());
    }

    return QJsonObject {
        { u"id"_s, id },
        { u"name"_s, name },
        { u"enabled"_s, enabled },
        { u"sourceKind"_s, CBridge::toString(sourceKind) },
        { u"whepUrl"_s, whepUrl.toString() },
        { u"username"_s, username },
        { u"srt"_s, srt.toJson() },
        { u"youtube"_s, youtube.toJson() },
        { u"preferredCodecs"_s, codecs },
        { u"audioEnabled"_s, audioEnabled },
        { u"reconnectInitialMs"_s, reconnectInitialMs },
        { u"reconnectMaxMs"_s, reconnectMaxMs },
        { u"sinks"_s, sinkArray },
    };
}

StreamConfig StreamConfig::fromJson(const QJsonObject &json)
{
    StreamConfig config;
    config.id = json.value(u"id"_s).toString();
    if (config.id.isEmpty()) {
        config.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    config.name = json.value(u"name"_s).toString();
    config.enabled = jsonBool(json, u"enabled"_s, true);
    // Missing sourceKind means an older document: it was always WHEP.
    bool ok = false;
    config.sourceKind = sourceKindFromString(json.value(u"sourceKind"_s).toString(), &ok);
    if (!ok) {
        config.sourceKind = SourceKind::Whep;
    }
    config.whepUrl = QUrl(json.value(u"whepUrl"_s).toString());
    config.username = json.value(u"username"_s).toString();
    config.srt = SrtSourceConfig::fromJson(json.value(u"srt"_s).toObject());
    config.youtube = YouTubeSourceConfig::fromJson(json.value(u"youtube"_s).toObject());
    config.audioEnabled = jsonBool(json, u"audioEnabled"_s, true);
    config.reconnectInitialMs = clampInt(jsonInt(json, u"reconnectInitialMs"_s, 500), 100, 60000);
    config.reconnectMaxMs =
        clampInt(jsonInt(json, u"reconnectMaxMs"_s, 15000), config.reconnectInitialMs, 300000);

    const QJsonArray codecs = json.value(u"preferredCodecs"_s).toArray();
    if (!codecs.isEmpty()) {
        config.preferredCodecs.clear();
        for (const QJsonValue &value : codecs) {
            const VideoCodec codec = videoCodecFromString(value.toString());
            if (codec != VideoCodec::Unknown && !config.preferredCodecs.contains(codec)) {
                config.preferredCodecs.append(codec);
            }
        }
    }
    if (config.preferredCodecs.isEmpty()) {
        config.preferredCodecs = { VideoCodec::H264, VideoCodec::H265 };
    }

    const QJsonArray sinkArray = json.value(u"sinks"_s).toArray();
    for (const QJsonValue &value : sinkArray) {
        config.sinks.append(SinkConfig::fromJson(value.toObject()));
    }

    return config;
}

StreamConfig StreamConfig::createDefault()
{
    StreamConfig config;
    config.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    config.name = u"New stream"_s;
    return config;
}

bool StreamConfig::hasSinkOfKind(SinkKind kind) const
{
    for (const SinkConfig &sink : sinks) {
        if (sink.kind == kind && sink.enabled) {
            return true;
        }
    }
    return false;
}

QJsonObject BridgeConfig::toJson() const
{
    QJsonArray streamArray;
    for (const StreamConfig &stream : streams) {
        streamArray.append(stream.toJson());
    }

    return QJsonObject {
        { u"version"_s, kSchemaVersion },
        { u"name"_s, name },
        { u"streams"_s, streamArray },
    };
}

std::optional<BridgeConfig> BridgeConfig::fromJson(const QJsonObject &json, QString *error)
{
    const int version = json.value(u"version"_s).toInt(0);
    if (version <= 0) {
        if (error) {
            *error = u"Missing or invalid \"version\" field."_s;
        }
        return std::nullopt;
    }
    if (version > kSchemaVersion) {
        if (error) {
            *error = u"Config schema version %1 is newer than this build supports (%2)."_s
                         .arg(version)
                         .arg(kSchemaVersion);
        }
        return std::nullopt;
    }

    BridgeConfig config;
    config.name = jsonString(json, u"name"_s, u"Untitled"_s);

    const QJsonArray streamArray = json.value(u"streams"_s).toArray();
    for (const QJsonValue &value : streamArray) {
        config.streams.append(StreamConfig::fromJson(value.toObject()));
    }

    return config;
}

bool BridgeConfig::save(const QString &path, QString *error) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = u"Could not open %1 for writing: %2"_s.arg(path, file.errorString());
        }
        return false;
    }

    const QJsonDocument document(toJson());
    if (file.write(document.toJson(QJsonDocument::Indented)) < 0) {
        if (error) {
            *error = u"Could not write %1: %2"_s.arg(path, file.errorString());
        }
        return false;
    }

    if (!file.commit()) {
        if (error) {
            *error = u"Could not commit %1: %2"_s.arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

std::optional<BridgeConfig> BridgeConfig::load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = u"Could not open %1: %2"_s.arg(path, file.errorString());
        }
        return std::nullopt;
    }

    QJsonParseError parseError {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = u"%1 is not valid JSON (offset %2): %3"_s.arg(path)
                         .arg(parseError.offset)
                         .arg(parseError.errorString());
        }
        return std::nullopt;
    }
    if (!document.isObject()) {
        if (error) {
            *error = u"%1 does not contain a JSON object."_s.arg(path);
        }
        return std::nullopt;
    }

    return fromJson(document.object(), error);
}

QStringList BridgeConfig::validateStream(const StreamConfig &candidate,
                                         const QString &excludeId) const
{
    QStringList problems;
    const QString label = candidate.name.trimmed().isEmpty() ? candidate.id : candidate.name;

    if (candidate.sourceKind == SourceKind::Whep) {
        if (!candidate.whepUrl.isValid() || candidate.whepUrl.scheme().isEmpty()) {
            problems.append(u"Stream \"%1\" has no valid WHEP URL."_s.arg(label));
        }
    } else if (candidate.sourceKind == SourceKind::Srt) {
        const SrtSourceConfig &srt = candidate.srt;
        if (srt.port == 0) {
            problems.append(u"Stream \"%1\": the SRT port must not be zero."_s.arg(label));
        }
        if (srt.mode == SrtSourceConfig::Mode::Caller && srt.host.trimmed().isEmpty()) {
            problems.append(u"Stream \"%1\" has an SRT caller source with no host."_s.arg(label));
        }
        // The SRT protocol caps the streamid at 512 characters; longer values are rejected
        // by the library before a connection is even attempted.
        if (srt.streamId.size() > 512) {
            problems.append(u"Stream \"%1\": the SRT stream ID must be at most 512 characters."_s.arg(label));
        }
    } else if (candidate.sourceKind == SourceKind::Youtube) {
        const YouTubeSourceConfig &youtube = candidate.youtube;
        if (!youtube.url.isValid() || youtube.url.scheme().isEmpty()) {
            problems.append(u"Stream \"%1\" has no valid YouTube URL."_s.arg(label));
        }
    }

    const auto enabledSinks = std::count_if(candidate.sinks.cbegin(), candidate.sinks.cend(),
        [](const SinkConfig &sink) { return sink.enabled; });
    if (candidate.enabled && enabledSinks == 0) {
        problems.append(u"Stream \"%1\" is enabled but has no enabled sink."_s.arg(label));
    }

    // Everything claimed by the other streams, so the candidate never clashes with itself.
    QSet<QString> endpoints;
    QSet<QString> rtspEndpoints;
    QSet<quint16> srtListenerPorts;
    QSet<QString> ndiNames;
    for (const StreamConfig &other : streams) {
        if (other.id == excludeId) {
            continue;
        }
        if (other.sourceKind == SourceKind::Srt && other.srt.mode == SrtSourceConfig::Mode::Listener) {
            srtListenerPorts.insert(other.srt.port);
        }
        for (const SinkConfig &sink : other.sinks) {
            if (!sink.enabled) {
                continue;
            }
            if (sink.kind == SinkKind::TsMulticast) {
                endpoints.insert(u"%1:%2"_s.arg(sink.ts.groupAddress).arg(sink.ts.port));
            } else if (sink.kind == SinkKind::RtpMulticast) {
                // Video port and the audio port right after it.
                endpoints.insert(u"%1:%2"_s.arg(sink.rtp.groupAddress).arg(int(sink.rtp.port)));
                endpoints.insert(u"%1:%2"_s.arg(sink.rtp.groupAddress).arg(int(sink.rtp.port) + 1));
            } else if (sink.kind == SinkKind::RtspUnicast) {
                rtspEndpoints.insert(u":%1/%2"_s.arg(sink.rtsp.port).arg(sink.rtsp.path));
            } else if (!sink.ndi.senderName.trimmed().isEmpty()) {
                ndiNames.insert(sink.ndi.senderName);
            }
        }
    }

    for (const SinkConfig &sink : candidate.sinks) {
        if (!sink.enabled) {
            continue;
        }

        switch (sink.kind) {
        case SinkKind::TsMulticast: {
            if (!isMulticastV4(sink.ts.groupAddress)) {
                problems.append(
                    u"Stream \"%1\": \"%2\" is not an IPv4 multicast address (224.0.0.0/4)."_s
                        .arg(label, sink.ts.groupAddress));
            }
            const QString endpoint = u"%1:%2"_s.arg(sink.ts.groupAddress).arg(sink.ts.port);
            if (endpoints.contains(endpoint)) {
                problems.append(
                    u"Multicast endpoint %1 is used by more than one sink."_s.arg(endpoint));
            }
            endpoints.insert(endpoint);
            break;
        }
        case SinkKind::RtpMulticast: {
            if (!isMulticastV4(sink.rtp.groupAddress)) {
                problems.append(
                    u"Stream \"%1\": \"%2\" is not an IPv4 multicast address (224.0.0.0/4)."_s
                        .arg(label, sink.rtp.groupAddress));
            }
            // Both the video port and the audio port after it must be free.
            const QString videoEndpoint = u"%1:%2"_s.arg(sink.rtp.groupAddress).arg(int(sink.rtp.port));
            const QString audioEndpoint = u"%1:%2"_s.arg(sink.rtp.groupAddress).arg(int(sink.rtp.port) + 1);
            if (endpoints.contains(videoEndpoint)) {
                problems.append(u"Multicast endpoint %1 is used by more than one sink."_s.arg(videoEndpoint));
            }
            if (endpoints.contains(audioEndpoint)) {
                problems.append(u"Multicast endpoint %1 is used by more than one sink."_s.arg(audioEndpoint));
            }
            endpoints.insert(videoEndpoint);
            endpoints.insert(audioEndpoint);
            break;
        }
        case SinkKind::RtspUnicast: {
            const QString endpoint = u":%1/%2"_s.arg(sink.rtsp.port).arg(sink.rtsp.path);
            if (rtspEndpoints.contains(endpoint)) {
                problems.append(u"RTSP endpoint %1 is used by more than one sink."_s.arg(endpoint));
            }
            rtspEndpoints.insert(endpoint);
            break;
        }
        case SinkKind::Ndi: {
            if (sink.ndi.senderName.trimmed().isEmpty()) {
                problems.append(u"Stream \"%1\" has an NDI sink with no sender name."_s.arg(label));
            } else if (ndiNames.contains(sink.ndi.senderName)) {
                problems.append(u"NDI sender name \"%1\" is used by more than one sink."_s
                                    .arg(sink.ndi.senderName));
            } else {
                ndiNames.insert(sink.ndi.senderName);
            }
            break;
        }
        }
    }

    if (candidate.sourceKind == SourceKind::Srt &&
        candidate.srt.mode == SrtSourceConfig::Mode::Listener &&
        srtListenerPorts.contains(candidate.srt.port)) {
        problems.append(u"SRT listener port %1 is used by more than one stream."_s
                            .arg(int(candidate.srt.port)));
    }

    return problems;
}

QStringList BridgeConfig::validate() const
{
    QStringList problems;
    QSet<QString> ids;

    for (const StreamConfig &stream : streams) {
        const QString label = stream.name.trimmed().isEmpty() ? stream.id : stream.name;
        if (ids.contains(stream.id)) {
            problems.append(u"Duplicate stream id for \"%1\"."_s.arg(label));
        }
        ids.insert(stream.id);

        problems.append(validateStream(stream, stream.id));
    }

    // Each cross-stream clash is reported once from either side.
    problems.removeDuplicates();
    return problems;
}

int BridgeConfig::indexOfStream(const QString &id) const
{
    for (int i = 0; i < streams.size(); ++i) {
        if (streams.at(i).id == id) {
            return i;
        }
    }
    return -1;
}

TsMulticastSinkConfig BridgeConfig::suggestMulticastEndpoint(bool audioPort) const
{
    // Endpoints claimed by every multicast sink, including both ports of an RTP sink.
    QSet<QString> used;
    for (const StreamConfig &stream : streams) {
        for (const SinkConfig &sink : stream.sinks) {
            if (!sink.enabled) continue;
            if (sink.kind == SinkKind::TsMulticast) {
                used.insert(u"%1:%2"_s.arg(sink.ts.groupAddress).arg(sink.ts.port));
            } else if (sink.kind == SinkKind::RtpMulticast) {
                used.insert(u"%1:%2"_s.arg(sink.rtp.groupAddress).arg(int(sink.rtp.port)));
                used.insert(u"%1:%2"_s.arg(sink.rtp.groupAddress).arg(int(sink.rtp.port) + 1));
            }
        }
    }

    TsMulticastSinkConfig candidate;
    for (int index = 1; index < 4096; ++index) {
        candidate.groupAddress = u"239.1.%1.%2"_s.arg(index / 254).arg((index % 254) + 1);
        candidate.port = quint16(5000 + (index * 2));
        const QString endpoint = u"%1:%2"_s.arg(candidate.groupAddress).arg(candidate.port);
        if (used.contains(endpoint)) {
            continue;
        }
        // An RTP sink also needs the next port for its audio stream.
        if (audioPort && used.contains(u"%1:%2"_s.arg(candidate.groupAddress, int(candidate.port) + 1))) {
            continue;
        }
        return candidate;
    }
    return candidate;
}

} // namespace CBridge
