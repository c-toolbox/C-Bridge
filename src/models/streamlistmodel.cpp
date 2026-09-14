#include "models/streamlistmodel.h"

#include "core/bridgeengine.h"

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
    };

    Q_EMIT dataChanged(index(0), index(int(m_streams.size()) - 1), liveRoles);
}

} // namespace CBridge
