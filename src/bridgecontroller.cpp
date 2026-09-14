#include "bridgecontroller.h"

#include "core/bridgeengine.h"
#include "models/streamlistmodel.h"

#include <QFileInfo>
#include <QSettings>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {
constexpr auto kLastConfigKey = "lastConfigPath";
constexpr auto kAutoLoadKey = "autoLoadLastConfig";
}

BridgeController::BridgeController(QObject *parent)
    : QObject(parent)
    , m_engine(new BridgeEngine(this))
    , m_model(new StreamListModel(this))
{
    m_model->setEngine(m_engine);

    connect(m_engine, &BridgeEngine::statsUpdated, this, [this] {
        m_model->refreshStats();
        Q_EMIT statsUpdated();
    });

    connect(m_engine, &BridgeEngine::streamStateChanged, this,
            [this](const QString &, StreamState) {
                m_model->refreshStats();
                Q_EMIT runningChanged();
            });

    connect(m_engine, &BridgeEngine::streamError, this,
            [this](const QString &streamId, const QString &message) {
                const int index = m_config.indexOfStream(streamId);
                const QString label =
                    index >= 0 ? m_config.streams.at(index).name : streamId;
                setStatusMessage(u"%1: %2"_s.arg(label, message));
            });
}

BridgeController::~BridgeController()
{
    m_engine->stopAll();
}

bool BridgeController::isRunning() const
{
    return m_engine->isRunning();
}

QString BridgeController::aggregateMbps() const
{
    return QString::number(m_engine->aggregateInputMbps(), 'f', 1);
}

void BridgeController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    if (!message.isEmpty()) {
        qInfo("%s", qUtf8Printable(message));
    }
    Q_EMIT statusMessageChanged();
}

void BridgeController::setDirty(bool dirty)
{
    if (m_dirty == dirty) {
        return;
    }
    m_dirty = dirty;
    Q_EMIT dirtyChanged();
}

void BridgeController::refreshModel()
{
    m_model->setStreams(m_config.streams);
}

void BridgeController::loadStartupConfig(const QString &explicitPath)
{
    if (!explicitPath.isEmpty()) {
        loadConfig(explicitPath);
        return;
    }

    QSettings settings;
    if (!settings.value(QString::fromLatin1(kAutoLoadKey), true).toBool()) {
        return;
    }

    const QString last = settings.value(QString::fromLatin1(kLastConfigKey)).toString();
    if (!last.isEmpty() && QFileInfo::exists(last)) {
        loadConfig(last);
    }
}

void BridgeController::newConfig()
{
    m_engine->stopAll();
    m_config = BridgeConfig {};
    m_configPath.clear();
    m_engine->setConfig(m_config);
    refreshModel();
    setDirty(false);
    setStatusMessage(u"New configuration"_s);
    Q_EMIT configChanged();
}

bool BridgeController::loadConfig(const QString &path)
{
    QString error;
    const auto loaded = BridgeConfig::load(path, &error);
    if (!loaded) {
        setStatusMessage(error);
        return false;
    }

    m_engine->stopAll();
    m_config = *loaded;
    m_configPath = path;
    m_engine->setConfig(m_config);
    refreshModel();
    setDirty(false);

    QSettings().setValue(QString::fromLatin1(kLastConfigKey), path);

    const QStringList problems = m_config.validate();
    setStatusMessage(problems.isEmpty()
        ? u"Loaded %1 (%2 streams)"_s.arg(QFileInfo(path).fileName()).arg(m_config.streams.size())
        : u"Loaded with %1 problem(s): %2"_s.arg(problems.size()).arg(problems.first()));

    Q_EMIT configChanged();
    return true;
}

bool BridgeController::saveConfig(const QString &path)
{
    QString error;
    if (!m_config.save(path, &error)) {
        setStatusMessage(error);
        return false;
    }

    m_configPath = path;
    setDirty(false);
    QSettings().setValue(QString::fromLatin1(kLastConfigKey), path);
    setStatusMessage(u"Saved %1"_s.arg(QFileInfo(path).fileName()));
    Q_EMIT configChanged();
    return true;
}

void BridgeController::startAll()
{
    const QStringList problems = m_config.validate();
    if (!problems.isEmpty()) {
        setStatusMessage(problems.first());
        return;
    }

    m_engine->setConfig(m_config);
    m_engine->startAll();
    setStatusMessage(u"Started"_s);
    Q_EMIT runningChanged();
}

void BridgeController::stopAll()
{
    m_engine->stopAll();
    setStatusMessage(u"Stopped"_s);
    Q_EMIT runningChanged();
}

void BridgeController::startStream(const QString &streamId)
{
    m_engine->setConfig(m_config);
    m_engine->startStream(streamId);
    Q_EMIT runningChanged();
}

void BridgeController::stopStream(const QString &streamId)
{
    m_engine->stopStream(streamId);
    Q_EMIT runningChanged();
}

void BridgeController::addStream(const QString &name, const QString &whepUrl)
{
    StreamConfig stream = StreamConfig::createDefault();
    if (!name.isEmpty()) {
        stream.name = name;
    }
    stream.whepUrl = QUrl(whepUrl);

    // Give every new stream a working multicast sink so it is runnable immediately.
    SinkConfig sink;
    sink.kind = SinkKind::TsMulticast;
    sink.ts = m_config.suggestMulticastEndpoint();
    stream.sinks.append(sink);

    m_config.streams.append(stream);
    refreshModel();
    setDirty(true);
    Q_EMIT configChanged();
}

void BridgeController::removeStream(const QString &streamId)
{
    const int index = m_config.indexOfStream(streamId);
    if (index < 0) {
        return;
    }

    m_engine->stopStream(streamId);
    m_config.streams.removeAt(index);
    refreshModel();
    setDirty(true);
    Q_EMIT configChanged();
}

void BridgeController::setStreamEnabled(const QString &streamId, bool enabled)
{
    const int index = m_config.indexOfStream(streamId);
    if (index < 0) {
        return;
    }

    m_config.streams[index].enabled = enabled;
    if (!enabled) {
        m_engine->stopStream(streamId);
    }
    refreshModel();
    setDirty(true);
}

QString BridgeController::mpvCommandFor(const QString &streamId) const
{
    const int index = m_config.indexOfStream(streamId);
    if (index < 0) {
        return {};
    }

    for (const SinkConfig &sink : m_config.streams.at(index).sinks) {
        if (sink.kind != SinkKind::TsMulticast || !sink.enabled) {
            continue;
        }
        // Without these, mpv buffers heavily and hides the low-latency benefit.
        return u"mpv udp://%1:%2 --profile=low-latency --cache=no --demuxer-lavf-o=fflags=+nobuffer"_s
            .arg(sink.ts.groupAddress)
            .arg(sink.ts.port);
    }
    return {};
}

QStringList BridgeController::validationProblems() const
{
    return m_config.validate();
}

} // namespace CBridge
