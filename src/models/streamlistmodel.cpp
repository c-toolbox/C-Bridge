/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "models/streamlistmodel.h"

#include "core/bridgeengine.h"

#include <QVariantList>
#include <QVariantMap>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

StreamListModel::StreamListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void StreamListModel::setEngine(BridgeEngine *engine)
{
    m_engine = engine;
}

void StreamListModel::setStreams(const QList<StreamConfig> &streams)
{
    beginResetModel();
    m_streams = streams;
    endResetModel();
}

int StreamListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_streams.size());
}

QVariant StreamListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_streams.size()) {
        return {};
    }

    const StreamConfig &stream = m_streams.at(index.row());
    const StreamStats stats = m_engine ? m_engine->statsFor(stream.id) : StreamStats {};

    switch (role) {
    case StreamIdRole:
        return stream.id;
    case NameRole:
        return stream.name;
    case EnabledRole:
        return stream.enabled;
    case SourceUrlRole:
        return stream.whepUrl.toString();
    case SinksRole: {
        QStringList descriptions;
        for (const SinkConfig &sink : stream.sinks) {
            if (sink.enabled) {
                descriptions.append(sink.describe());
            }
        }
        return descriptions.join(u", "_s);
    }
    case SinkStatsRole: {
        const QList<SinkStats> live = m_engine ? m_engine->sinkStatsFor(stream.id) : QList<SinkStats> {};
        QVariantList rows;
        // Falls back to the configured sinks so a stopped stream still lists them.
        if (live.isEmpty()) {
            for (const SinkConfig &sink : stream.sinks) {
                rows.append(QVariantMap {
                    { u"description"_s, sink.describe() },
                    { u"open"_s, false },
                    { u"mbps"_s, u"0.00"_s },
                });
            }
            return rows;
        }
        for (const SinkStats &sink : live) {
            rows.append(QVariantMap {
                { u"description"_s, sink.description },
                { u"open"_s, sink.open },
                { u"mbps"_s, QString::number(sink.outputMbps, 'f', 2) },
            });
        }
        return rows;
    }
    case StateRole:
        return CBridge::toString(stats.state);
    case ResolutionRole:
        return stats.width > 0 ? u"%1x%2"_s.arg(stats.width).arg(stats.height) : u"-"_s;
    case CodecRole:
        return CBridge::toString(stats.videoCodec);
    case MbpsRole:
        return QString::number(stats.inputMbps, 'f', 2);
    case DroppedRole:
        return qulonglong(stats.droppedFrames);
    case QueueDepthRole:
        return stats.queueDepth;
    case ReconnectsRole:
        return stats.reconnectCount;
    case LastErrorRole:
        return stats.lastError;
    default:
        return {};
    }
}

QHash<int, QByteArray> StreamListModel::roleNames() const
{
    return {
        { StreamIdRole, "streamId" },
        { NameRole, "name" },
        { EnabledRole, "enabled" },
        { SourceUrlRole, "sourceUrl" },
        { SinksRole, "sinks" },
        { SinkStatsRole, "sinkStats" },
        { StateRole, "state" },
        { ResolutionRole, "resolution" },
        { CodecRole, "codec" },
        { MbpsRole, "mbps" },
        { DroppedRole, "dropped" },
        { QueueDepthRole, "queueDepth" },
        { ReconnectsRole, "reconnects" },
        { LastErrorRole, "lastError" },
    };
}

void StreamListModel::refreshStats()
{
    if (m_streams.isEmpty()) {
        return;
    }

    static const QList<int> liveRoles {
        StateRole, ResolutionRole, CodecRole, MbpsRole,
        DroppedRole, QueueDepthRole, ReconnectsRole, LastErrorRole,
        SinkStatsRole,
    };

    Q_EMIT dataChanged(index(0), index(int(m_streams.size()) - 1), liveRoles);
}

} // namespace CBridge
