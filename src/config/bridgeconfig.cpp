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

bool isMulticastV4(const QString &address)
{
    QHostAddress host;
    if (!host.setAddress(address) || host.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }
    const quint32 raw = host.toIPv4Address();
    return (raw >> 28) == 0xE; // 224.0.0.0/4
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
    config.groupAddress = json.value(u"groupAddress"_s).toString(config.groupAddress);
    config.port = quint16(clampInt(json.value(u"port"_s).toInt(config.port), 1, 65535));
    config.ttl = clampInt(json.value(u"ttl"_s).toInt(config.ttl), 1, 255);
    config.localAddress = json.value(u"localAddress"_s).toString();
    config.packetSize = clampInt(json.value(u"packetSize"_s).toInt(config.packetSize), 188, 65535);
    config.patPeriodMs = clampInt(json.value(u"patPeriodMs"_s).toInt(config.patPeriodMs), 10, 5000);
    config.pcrPeriodMs = clampInt(json.value(u"pcrPeriodMs"_s).toInt(config.pcrPeriodMs), 10, 500);
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
    config.fpsDen = std::max(1, json.value(u"fpsDen"_s).toInt(1));
    config.audioEnabled = json.value(u"audioEnabled"_s).toBool(true);
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
    config.enabled = json.value(u"enabled"_s).toBool(true);
    config.ts = TsMulticastSinkConfig::fromJson(json.value(u"tsMulticast"_s).toObject());
    config.ndi = NdiSinkConfig::fromJson(json.value(u"ndi"_s).toObject());
    return config;
}

QString SinkConfig::describe() const
{
    switch (kind) {
    case SinkKind::TsMulticast:
        return u"TS %1:%2"_s.arg(ts.groupAddress).arg(ts.port);
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
        { u"whepUrl"_s, whepUrl.toString() },
        { u"username"_s, username },
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
    config.enabled = json.value(u"enabled"_s).toBool(true);
    config.whepUrl = QUrl(json.value(u"whepUrl"_s).toString());
    config.username = json.value(u"username"_s).toString();
    config.audioEnabled = json.value(u"audioEnabled"_s).toBool(true);
    config.reconnectInitialMs =
        clampInt(json.value(u"reconnectInitialMs"_s).toInt(500), 100, 60000);
    config.reconnectMaxMs =
        clampInt(json.value(u"reconnectMaxMs"_s).toInt(15000), config.reconnectInitialMs, 300000);

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
    config.name = json.value(u"name"_s).toString(u"Untitled"_s);

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

QStringList BridgeConfig::validate() const
{
    QStringList problems;
    QSet<QString> endpoints;
    QSet<QString> ndiNames;
    QSet<QString> ids;

    for (const StreamConfig &stream : streams) {
        const QString label = stream.name.isEmpty() ? stream.id : stream.name;

        if (ids.contains(stream.id)) {
            problems.append(u"Duplicate stream id for \"%1\"."_s.arg(label));
        }
        ids.insert(stream.id);
        if (!stream.whepUrl.isValid() || stream.whepUrl.scheme().isEmpty()) {
            problems.append(u"Stream \"%1\" has no valid WHEP URL."_s.arg(label));
        }

        const auto enabledSinks = std::count_if(stream.sinks.cbegin(), stream.sinks.cend(),
            [](const SinkConfig &sink) { return sink.enabled; });
        if (stream.enabled && enabledSinks == 0) {
            problems.append(u"Stream \"%1\" is enabled but has no enabled sink."_s.arg(label));
        }

        for (const SinkConfig &sink : stream.sinks) {
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
                        u"Multicast endpoint %1 is used by more than one stream."_s.arg(endpoint));
                }
                endpoints.insert(endpoint);
                break;
            }
            case SinkKind::Ndi: {
                if (sink.ndi.senderName.trimmed().isEmpty()) {
                    problems.append(u"Stream \"%1\" has an NDI sink with no sender name."_s
                                        .arg(label));
                } else if (ndiNames.contains(sink.ndi.senderName)) {
                    problems.append(u"NDI sender name \"%1\" is used by more than one stream."_s
                                        .arg(sink.ndi.senderName));
                } else {
                    ndiNames.insert(sink.ndi.senderName);
                }
                break;
            }
            }
        }
    }

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

TsMulticastSinkConfig BridgeConfig::suggestMulticastEndpoint() const
{
    QSet<QString> used;
    for (const StreamConfig &stream : streams) {
        for (const SinkConfig &sink : stream.sinks) {
            if (sink.kind == SinkKind::TsMulticast) {
                used.insert(u"%1:%2"_s.arg(sink.ts.groupAddress).arg(sink.ts.port));
            }
        }
    }

    TsMulticastSinkConfig candidate;
    for (int index = 1; index < 4096; ++index) {
        candidate.groupAddress = u"239.1.%1.%2"_s.arg(index / 254).arg((index % 254) + 1);
        candidate.port = quint16(5000 + (index * 2));
        const QString endpoint = u"%1:%2"_s.arg(candidate.groupAddress).arg(candidate.port);
        if (!used.contains(endpoint)) {
            return candidate;
        }
    }
    return candidate;
}

} // namespace CBridge
