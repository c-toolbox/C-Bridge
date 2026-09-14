#pragma once

#include "config/bridgeconfig.h"

#include <QAbstractListModel>

namespace CBridge {

class BridgeEngine;

/// Exposes the configured streams plus their live counters to QML.
class StreamListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        StreamIdRole = Qt::UserRole + 1,
        NameRole,
        EnabledRole,
        SourceUrlRole,
        SinksRole,
        StateRole,
        ResolutionRole,
        CodecRole,
        MbpsRole,
        DroppedRole,
        QueueDepthRole,
        ReconnectsRole,
        LastErrorRole,
    };

    explicit StreamListModel(QObject *parent = nullptr);

    void setEngine(BridgeEngine *engine);
    void setStreams(const QList<StreamConfig> &streams);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Re-emits dataChanged for the live counter roles only.
    void refreshStats();

private:
    QList<StreamConfig> m_streams;
    BridgeEngine *m_engine = nullptr;
};

} // namespace CBridge
