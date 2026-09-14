#pragma once

#include "bridgecontroller.h"

#include <QObject>
#include <QString>
#include <QUrl>

class QGuiApplication;
class QQmlApplicationEngine;
class KAboutData;

namespace CBridge {

/// Owns the Qt application objects and the QML engine, and brokers process-wide
/// services (logging, version strings, path helpers) to QML.
class BridgeApplication : public QObject
{
    Q_OBJECT
    Q_PROPERTY(CBridge::BridgeController *controller READ controller CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
    Q_PROPERTY(QString ffmpegVersion READ ffmpegVersion CONSTANT)
    Q_PROPERTY(QString libdatachannelVersion READ libdatachannelVersion CONSTANT)
    Q_PROPERTY(QString ndiVersion READ ndiVersion CONSTANT)

public:
    BridgeApplication(int &argc, char **argv);
    ~BridgeApplication() override;

    int run();

    BridgeController *controller() const { return m_controller; }

    QString version() const;
    QString qtVersion() const;
    QString ffmpegVersion() const;
    QString libdatachannelVersion() const;
    QString ndiVersion() const;

    Q_INVOKABLE QUrl pathToUrl(const QString &path) const;
    Q_INVOKABLE QString urlToPath(const QUrl &url) const;

    /// Resolves a path inside the deployed data/ directory, searching the install
    /// layout first and then the source tree so the app runs from a build folder.
    Q_INVOKABLE QString dataPath(const QString &relativePath) const;

    Q_INVOKABLE void copyToClipboard(const QString &text) const;

private:
    void installMessageHandler();

    QGuiApplication *m_app = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
    KAboutData *m_aboutData = nullptr;
    BridgeController *m_controller = nullptr;
};

} // namespace CBridge
