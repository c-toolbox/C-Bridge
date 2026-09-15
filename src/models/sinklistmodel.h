/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"

#include <QAbstractListModel>

namespace CBridge {

/// Editable view over one stream's sinks. Every field is a writable role so QML
/// delegates can assign `model.port = ...` directly.
class SinkListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        KindRole = Qt::UserRole + 1,
        EnabledRole,
        SummaryRole,

        GroupAddressRole,
        PortRole,
        TtlRole,
        LocalAddressRole,
        PacketSizeRole,
        PatPeriodMsRole,
        PcrPeriodMsRole,
        TsUrlRole,

        SenderNameRole,
        TargetWidthRole,
        TargetHeightRole,
        FpsNumRole,
        FpsDenRole,
        NdiAudioEnabledRole,
    };

    explicit SinkListModel(QObject *parent = nullptr);

    void setSinks(const QList<SinkConfig> &sinks);
    const QList<SinkConfig> &sinks() const { return m_sinks; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addSink(const QString &kind);
    Q_INVOKABLE void removeSink(int row);
    Q_INVOKABLE void setKind(int row, const QString &kind);

    /// Role-name setter for delegates that cannot reach the attached `model` object.
    Q_INVOKABLE bool set(int row, const QString &roleName, const QVariant &value);

Q_SIGNALS:
    void countChanged();
    void sinksChanged();

private:
    QList<SinkConfig> m_sinks;
};

} // namespace CBridge
