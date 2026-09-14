#pragma once

#include "config/bridgeconfig.h"
#include "models/streamlistmodel.h"

#include <QObject>
#include <QString>
#include <QUrl>

namespace CBridge {

class BridgeEngine;

/// The single object QML talks to. Owns the config document and drives the engine.
class BridgeController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString configPath READ configPath NOTIFY configChanged)
    Q_PROPERTY(QString configName READ configName NOTIFY configChanged)
    Q_PROPERTY(bool dirty READ isDirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(QString aggregateMbps READ aggregateMbps NOTIFY statsUpdated)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(CBridge::StreamListModel *streams READ streams CONSTANT)

public:
    explicit BridgeController(QObject *parent = nullptr);
    ~BridgeController() override;

    QString configPath() const { return m_configPath; }
    QString configName() const { return m_config.name; }
    bool isDirty() const { return m_dirty; }
    bool isRunning() const;
    QString aggregateMbps() const;
    QString statusMessage() const { return m_statusMessage; }
    StreamListModel *streams() const { return m_model; }

    /// Applies the startup config selection: an explicit path wins, otherwise the
    /// last used one when auto-load is enabled.
    void loadStartupConfig(const QString &explicitPath);

    Q_INVOKABLE void newConfig();
    Q_INVOKABLE bool loadConfig(const QString &path);
    Q_INVOKABLE bool saveConfig(const QString &path);

    Q_INVOKABLE void startAll();
    Q_INVOKABLE void stopAll();
    Q_INVOKABLE void startStream(const QString &streamId);
    Q_INVOKABLE void stopStream(const QString &streamId);

    Q_INVOKABLE void addStream(const QString &name, const QString &whepUrl);
    Q_INVOKABLE void removeStream(const QString &streamId);
    Q_INVOKABLE void setStreamEnabled(const QString &streamId, bool enabled);

    /// A ready-to-paste low-latency mpv command for a stream's multicast sink.
    Q_INVOKABLE QString mpvCommandFor(const QString &streamId) const;

    Q_INVOKABLE QStringList validationProblems() const;

Q_SIGNALS:
    void configChanged();
    void dirtyChanged();
    void runningChanged();
    void statsUpdated();
    void statusMessageChanged();

private:
    void setStatusMessage(const QString &message);
    void setDirty(bool dirty);
    void refreshModel();

    BridgeConfig m_config;
    QString m_configPath;
    bool m_dirty = false;
    QString m_statusMessage;

    BridgeEngine *m_engine = nullptr;
    StreamListModel *m_model = nullptr;
};

} // namespace CBridge
