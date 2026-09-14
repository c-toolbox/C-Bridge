#include "bridgeapplication.h"

#include "bridgecontroller.h"
#include "cbridgeversion.h"
#include "ndi/ndiruntime.h"

#include <KAboutData>
#include <KColorSchemeManager>
#include <KLocalizedString>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>

#include <rtc/version.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/ffversion.h>
}

#ifdef CBRIDGE_NDI_SUPPORT
#include <Processing.NDI.Lib.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace {

constexpr qint64 kMaxLogBytes = 8 * 1024 * 1024;

QMutex g_logMutex;
QFile g_logFile;

void rotateIfNeeded(const QString &path)
{
    QFileInfo info(path);
    if (info.exists() && info.size() > kMaxLogBytes) {
        const QString previous = path + ".1"_L1;
        QFile::remove(previous);
        QFile::rename(path, previous);
    }
}

void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    const char *level = "INFO ";
    switch (type) {
    case QtDebugMsg:    level = "DEBUG"; break;
    case QtInfoMsg:     level = "INFO "; break;
    case QtWarningMsg:  level = "WARN "; break;
    case QtCriticalMsg: level = "ERROR"; break;
    case QtFatalMsg:    level = "FATAL"; break;
    }

    const QString line = u"%1 %2 %3"_s
        .arg(QDateTime::currentDateTime().toString(u"yyyy-MM-dd HH:mm:ss.zzz"_s),
             QString::fromLatin1(level),
             message);

    QMutexLocker locker(&g_logMutex);
    if (g_logFile.isOpen()) {
        QTextStream stream(&g_logFile);
        stream << line << Qt::endl;
    }

    QTextStream console(stderr);
    console << line << Qt::endl;

    if (type == QtFatalMsg) {
        abort();
    }
}

} // namespace

namespace CBridge {

BridgeApplication::BridgeApplication(int &argc, char **argv)
{
    m_app = new QGuiApplication(argc, argv);
    QCoreApplication::setOrganizationName(u"C-Toolbox"_s);
    QCoreApplication::setOrganizationDomain(u"ctoolbox.org"_s);
    QCoreApplication::setApplicationName(u"C-Bridge"_s);
    QCoreApplication::setApplicationVersion(QString::fromLatin1(CBRIDGE_VERSION_STRING));

    installMessageHandler();

    // Match C-Slice's look and feel: explicit KDE Quick style with a Fusion fallback.
    QQuickStyle::setStyle(u"org.kde.desktop"_s);
    QQuickStyle::setFallbackStyle(u"Fusion"_s);

    m_aboutData = new KAboutData(
        u"c-bridge"_s,
        i18n("C-Bridge"),
        QString::fromLatin1(CBRIDGE_VERSION_STRING),
        i18n("Multi-stream WebRTC gateway with MPEG-TS multicast and NDI output"),
        KAboutLicense::GPL_V3);
    KAboutData::setApplicationData(*m_aboutData);

    // Same breeze icon theme as C-Slice.
    QIcon::setFallbackThemeName(u"breeze"_s);
    QIcon::setThemeName(u"breeze"_s);

    // Same window icon as C-Slice (embedded via data/images/images.qrc; Qt 6
    // auto-registers qrc resources, so no explicit init call is needed).
    m_app->setWindowIcon(QIcon(u":/C_transparent.png"_s));

    // Activate the Breeze Dark color scheme, same as C-Slice. The .colors file is
    // installed to <appdir>/data/color-schemes (see data/CMakeLists.txt), which
    // KColorSchemeManager finds via QStandardPaths::GenericDataLocation on Windows.
    KColorSchemeManager *schemes = KColorSchemeManager::instance();
    schemes->activateScheme(schemes->indexForScheme(u"Breeze Dark"_s));

    m_engine = new QQmlApplicationEngine(this);
    m_engine->rootContext()->setContextProperty(u"app"_s, this);
}
BridgeApplication::~BridgeApplication()
{
    delete m_engine;
    delete m_aboutData;
    delete m_app;

    QMutexLocker locker(&g_logMutex);
    if (g_logFile.isOpen()) {
        g_logFile.close();
    }
}

void BridgeApplication::installMessageHandler()
{
    const QString logDir = dataPath(u"log"_s);
    QDir().mkpath(logDir);

    const QString logPath = logDir + u"/cbridge.log"_s;
    rotateIfNeeded(logPath);

    g_logFile.setFileName(logPath);
    if (!g_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream(stderr) << u"Could not open log file: "_s << logPath << Qt::endl;
    }

    qInstallMessageHandler(messageHandler);
}

int BridgeApplication::run()
{
    qInfo() << "C-Bridge" << version() << "starting";
    qInfo() << "FFmpeg:" << ffmpegVersion();
    qInfo() << "libdatachannel:" << libdatachannelVersion();
    qInfo() << "NDI:" << ndiVersion();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        i18n("Multi-stream WebRTC gateway with MPEG-TS multicast and NDI output"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption configOption(
        { u"c"_s, u"config"_s },
        i18n("Configuration file to load at startup."),
        u"path"_s);
    parser.addOption(configOption);

    const QCommandLineOption autostartOption(
        u"autostart"_s, i18n("Start every enabled stream once the config is loaded."));
    parser.addOption(autostartOption);

    parser.process(*m_app);

    m_controller = new BridgeController(this);
    m_engine->rootContext()->setContextProperty(u"controller"_s, m_controller);
    m_controller->loadStartupConfig(parser.value(configOption));

    m_engine->loadFromModule(u"org.ctoolbox.cbridge"_s, u"Main"_s);
    if (m_engine->rootObjects().isEmpty()) {
        qCritical() << "Failed to load the root QML component";
        return 1;
    }

    if (parser.isSet(autostartOption)) {
        m_controller->startAll();
    }

    return QGuiApplication::exec();
}

QString BridgeApplication::version() const
{
    return QString::fromLatin1(CBRIDGE_VERSION_STRING);
}

QString BridgeApplication::qtVersion() const
{
    return QString::fromLatin1(qVersion());
}

QString BridgeApplication::ffmpegVersion() const
{
    return u"%1 (avcodec %2, avformat %3)"_s
        .arg(QString::fromLatin1(FFMPEG_VERSION),
             QString::fromLatin1(AV_STRINGIFY(LIBAVCODEC_VERSION)),
             QString::fromLatin1(AV_STRINGIFY(LIBAVFORMAT_VERSION)));
}

QString BridgeApplication::libdatachannelVersion() const
{
    return QString::fromLatin1(RTC_VERSION);
}

QString BridgeApplication::ndiVersion() const
{
#ifdef CBRIDGE_NDI_SUPPORT
    NdiRuntime &runtime = NdiRuntime::instance();
    if (runtime.ensureLoaded()) {
        return runtime.version();
    }
    return u"unavailable: %1"_s.arg(runtime.lastError());
#else
    return u"not built in"_s;
#endif
}

QUrl BridgeApplication::pathToUrl(const QString &path) const
{
    return QUrl::fromLocalFile(path);
}
QString BridgeApplication::urlToPath(const QUrl &url) const
{
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}

void BridgeApplication::copyToClipboard(const QString &text) const
{
    if (!text.isEmpty()) {
        QGuiApplication::clipboard()->setText(text);
    }
}

QString BridgeApplication::dataPath(const QString &relativePath) const
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + u"/data"_s,
        appDir + u"/../data"_s,
        appDir + u"/../../data"_s,
        QString::fromUtf8(CBRIDGE_SOURCE_DATA_DIR),
    };

    for (const QString &base : candidates) {
        if (QFileInfo::exists(base)) {
            return QDir::cleanPath(base + u"/"_s + relativePath);
        }
    }

    return QDir::cleanPath(appDir + u"/data/"_s + relativePath);
}

} // namespace CBridge
