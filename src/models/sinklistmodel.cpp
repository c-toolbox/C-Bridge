/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "models/sinklistmodel.h"

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

SinkListModel::SinkListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void SinkListModel::setSinks(const QList<SinkConfig> &sinks)
{
    beginResetModel();
    m_sinks = sinks;
    endResetModel();
    Q_EMIT countChanged();
    Q_EMIT sinksChanged();
}

int SinkListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_sinks.size());
}

QVariant SinkListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_sinks.size()) {
        return {};
    }

    const SinkConfig &sink = m_sinks.at(index.row());

    switch (role) {
    case KindRole:
        return CBridge::toString(sink.kind);
    case EnabledRole:
        return sink.enabled;
    case SummaryRole:
        return sink.describe();

    case GroupAddressRole:
        return sink.ts.groupAddress;
    case PortRole:
        return int(sink.ts.port);
    case TtlRole:
        return sink.ts.ttl;
    case LocalAddressRole:
        return sink.ts.localAddress;
    case PacketSizeRole:
        return sink.ts.packetSize;
    case PatPeriodMsRole:
        return sink.ts.patPeriodMs;
    case PcrPeriodMsRole:
        return sink.ts.pcrPeriodMs;
    case TsUrlRole:
        return sink.ts.url();

    case RtpGroupAddressRole:
        return sink.rtp.groupAddress;
    case RtpPortRole:
        return int(sink.rtp.port);
    case RtpTtlRole:
        return sink.rtp.ttl;
    case RtpLocalAddressRole:
        return sink.rtp.localAddress;
    case RtpPacketSizeRole:
        return sink.rtp.packetSize;
    case RtpVideoUrlRole:
        return sink.rtp.videoUrl();
    case RtpAudioUrlRole:
        return sink.rtp.audioUrl();

    case RtspPortRole:
        return sink.rtsp.port;
    case RtspPathRole:
        return sink.rtsp.path;
    case RtspLocalAddressRole:
        return sink.rtsp.localAddress;
    case RtspUrlRole:
        return sink.rtsp.url();

    case SenderNameRole:
        return sink.ndi.senderName;
    case TargetWidthRole:
        return sink.ndi.targetWidth;
    case TargetHeightRole:
        return sink.ndi.targetHeight;
    case FpsNumRole:
        return sink.ndi.fpsNum;
    case FpsDenRole:
        return sink.ndi.fpsDen;
    case NdiAudioEnabledRole:
        return sink.ndi.audioEnabled;

    default:
        return {};
    }
}

bool SinkListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_sinks.size()) {
        return false;
    }

    SinkConfig &sink = m_sinks[index.row()];
    QList<int> changed { role };

    switch (role) {
    case KindRole: {
        bool ok = false;
        const SinkKind kind = sinkKindFromString(value.toString(), &ok);
        if (!ok || kind == sink.kind) {
            return false;
        }
        sink.kind = kind;
        break;
    }
    case EnabledRole:
        sink.enabled = value.toBool();
        break;

    case GroupAddressRole:
        sink.ts.groupAddress = value.toString();
        changed.append(TsUrlRole);
        break;
    case PortRole:
        sink.ts.port = quint16(qBound(1, value.toInt(), 65535));
        changed.append(TsUrlRole);
        break;
    case TtlRole:
        sink.ts.ttl = qBound(1, value.toInt(), 255);
        changed.append(TsUrlRole);
        break;
    case LocalAddressRole:
        sink.ts.localAddress = value.toString();
        changed.append(TsUrlRole);
        break;
    case PacketSizeRole:
        sink.ts.packetSize = qBound(188, value.toInt(), 65535);
        changed.append(TsUrlRole);
        break;
    case PatPeriodMsRole:
        sink.ts.patPeriodMs = qBound(10, value.toInt(), 5000);
        break;
    case PcrPeriodMsRole:
        sink.ts.pcrPeriodMs = qBound(10, value.toInt(), 500);
        break;

    case RtpGroupAddressRole:
        sink.rtp.groupAddress = value.toString();
        changed.append(RtpVideoUrlRole);
        changed.append(RtpAudioUrlRole);
        break;
    case RtpPortRole:
        // The audio stream uses port + 1, so the video port may not be the last one.
        sink.rtp.port = quint16(qBound(1, value.toInt(), 65534));
        changed.append(RtpVideoUrlRole);
        changed.append(RtpAudioUrlRole);
        break;
    case RtpTtlRole:
        sink.rtp.ttl = qBound(1, value.toInt(), 255);
        break;
    case RtpLocalAddressRole:
        sink.rtp.localAddress = value.toString();
        break;
    case RtpPacketSizeRole:
        sink.rtp.packetSize = qBound(64, value.toInt(), 65535);
        break;

    case RtspPortRole:
        sink.rtsp.port = qBound(1, value.toInt(), 65535);
        changed.append(RtspUrlRole);
        break;
    case RtspPathRole:
        sink.rtsp.path = value.toString();
        changed.append(RtspUrlRole);
        break;
    case RtspLocalAddressRole:
        sink.rtsp.localAddress = value.toString();
        changed.append(RtspUrlRole);
        break;

    case SenderNameRole:
        sink.ndi.senderName = value.toString();
        break;
    case TargetWidthRole:
        sink.ndi.targetWidth = std::max(0, value.toInt());
        break;
    case TargetHeightRole:
        sink.ndi.targetHeight = std::max(0, value.toInt());
        break;
    case FpsNumRole:
        sink.ndi.fpsNum = std::max(0, value.toInt());
        break;
    case FpsDenRole:
        sink.ndi.fpsDen = std::max(1, value.toInt());
        break;
    case NdiAudioEnabledRole:
        sink.ndi.audioEnabled = value.toBool();
        break;

    default:
        return false;
    }

    changed.append(SummaryRole);
    Q_EMIT dataChanged(index, index, changed);
    Q_EMIT sinksChanged();
    return true;
}

Qt::ItemFlags SinkListModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return QAbstractListModel::flags(index) | Qt::ItemIsEditable;
}

QHash<int, QByteArray> SinkListModel::roleNames() const
{
    return {
        { KindRole, "kind" },
        { EnabledRole, "enabled" },
        { SummaryRole, "summary" },

        { GroupAddressRole, "groupAddress" },
        { PortRole, "port" },
        { TtlRole, "ttl" },
        { LocalAddressRole, "localAddress" },
        { PacketSizeRole, "packetSize" },
        { PatPeriodMsRole, "patPeriodMs" },
        { PcrPeriodMsRole, "pcrPeriodMs" },
        { TsUrlRole, "tsUrl" },

        { RtpGroupAddressRole, "rtpGroupAddress" },
        { RtpPortRole, "rtpPort" },
        { RtpTtlRole, "rtpTtl" },
        { RtpLocalAddressRole, "rtpLocalAddress" },
        { RtpPacketSizeRole, "rtpPacketSize" },
        { RtpVideoUrlRole, "rtpVideoUrl" },
        { RtpAudioUrlRole, "rtpAudioUrl" },

        { RtspPortRole, "rtspPort" },
        { RtspPathRole, "rtspPath" },
        { RtspLocalAddressRole, "rtspLocalAddress" },
        { RtspUrlRole, "rtspUrl" },

        { SenderNameRole, "senderName" },
        { TargetWidthRole, "targetWidth" },
        { TargetHeightRole, "targetHeight" },
        { FpsNumRole, "fpsNum" },
        { FpsDenRole, "fpsDen" },
        { NdiAudioEnabledRole, "ndiAudioEnabled" },
    };
}

void SinkListModel::addSink(const QString &kind)
{
    bool ok = false;
    const SinkKind resolved = sinkKindFromString(kind, &ok);
    if (!ok) {
        return;
    }

    SinkConfig sink;
    sink.kind = resolved;

    const int row = int(m_sinks.size());
    beginInsertRows({}, row, row);
    m_sinks.append(sink);
    endInsertRows();

    Q_EMIT countChanged();
    Q_EMIT sinksChanged();
}

void SinkListModel::removeSink(int row)
{
    if (row < 0 || row >= m_sinks.size()) {
        return;
    }

    beginRemoveRows({}, row, row);
    m_sinks.removeAt(row);
    endRemoveRows();

    Q_EMIT countChanged();
    Q_EMIT sinksChanged();
}

// Both sub-structs stay populated, so switching kind and back is lossless.
void SinkListModel::setKind(int row, const QString &kind)
{
    setData(index(row), kind, KindRole);
}

bool SinkListModel::set(int row, const QString &roleName, const QVariant &value)
{
    const QByteArray needle = roleName.toUtf8();
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it) {
        if (it.value() == needle) {
            return setData(index(row), value, it.key());
        }
    }
    return false;
}

} // namespace CBridge
