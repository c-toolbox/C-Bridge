/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bridgecontroller.h"

#include "cbridgesettings.h"
#include "core/bridgeengine.h"
#include "models/streamlistmodel.h"
#include "ndi/ndiruntime.h"

#include <KConfigGroup>

#include <QFileInfo>
#include <QSettings>
#include <QUuid>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {
// The last-config path used to live in plain QSettings before preferences moved to
// KConfig; read it as a fallback so an upgrade does not lose the auto-loaded document.
constexpr auto kLegacyLastConfigKey = "lastConfigPath";

QString storedLastConfigPath()
{
    const QString last = CBridgeSettings::lastConfigPath();
    if (!last.isEmpty()) {
        return last;
    }
    QSettings settings;
    return settings.value(QString::fromLatin1(kLegacyLastConfigKey)).toString();
}
}

BridgeController::BridgeController(QObject *parent)
    : QObject(parent)
    , m_engine(new BridgeEngine(this))
    , m_model(new StreamListModel(this))
    , m_draft(new StreamDraft(this))
{
    m_model->setEngine(m_engine);

    connect(m_draft, &StreamDraft::changed, this, &BridgeController::recomputeDraftProblems);

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

QString BridgeController::aggregateOutMbps() const
{
    return QString::number(m_engine->aggregateOutputMbps(), 'f', 1);
}

bool BridgeController::isNdiAvailable() const
{
#ifdef CBRIDGE_NDI_SUPPORT
    return NdiRuntime::instance().ensureLoaded();
#else
    return false;
#endif
}

QString BridgeController::ndiStatus() const
{
#ifdef CBRIDGE_NDI_SUPPORT
    NdiRuntime &runtime = NdiRuntime::instance();
    if (runtime.ensureLoaded()) {
        return u"NDI runtime %1"_s.arg(runtime.version());
    }
    const QString error = runtime.lastError();
    return error.isEmpty() ? u"The NDI runtime could not be loaded."_s : error;
#else
    return u"This build was compiled without NDI support."_s;
#endif
}

QStringList BridgeController::sinkKindNames() const
{
    return { CBridge::toString(SinkKind::TsMulticast), CBridge::toString(SinkKind::Ndi) };
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
    // Resolution order: --stream-config, the configured startup document, then the last
    // opened one when auto-load is enabled.
    QString path = explicitPath;
    if (path.isEmpty()) {
        const QString configured = CBridgeSettings::configPath();
        if (!configured.isEmpty() && QFileInfo::exists(configured)) {
            path = configured;
        } else if (CBridgeSettings::autoLoadLastConfig()) {
            const QString last = storedLastConfigPath();
            if (!last.isEmpty() && QFileInfo::exists(last)) {
                path = last;
            }
        }
    }

    if (path.isEmpty() || !loadConfig(path)) {
        return;
    }

    startStreamsOnLoad();
}

void BridgeController::startStreamsOnLoad()
{
    // No global validation gate here: the load status already lists document problems and
    // each stream reports its own error if it cannot run.
    const bool startAll = CBridgeSettings::startOnLoad();

    m_engine->setConfig(m_config);
    int started = 0;
    for (const StreamConfig &stream : m_config.streams) {
        if (!stream.enabled || !(startAll || streamAutoStart(stream.id))) {
            continue;
        }
        m_engine->startStream(stream.id);
        ++started;
    }

    if (started > 0) {
        setStatusMessage(u"Started %1 stream(s) on load"_s.arg(started));
        Q_EMIT runningChanged();
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

    CBridgeSettings::setLastConfigPath(path);
    CBridgeSettings::self()->save();

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
    CBridgeSettings::setLastConfigPath(path);
    CBridgeSettings::self()->save();
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
    beginEditStream({});
    if (!name.isEmpty()) {
        m_draft->setName(name);
    }
    m_draft->setWhepUrl(whepUrl);
    commitEdit();
}

void BridgeController::beginEditStream(const QString &streamId)
{
    const int index = m_config.indexOfStream(streamId);
    m_draftIsNew = index < 0;

    if (!m_draftIsNew) {
        m_draft->load(m_config.streams.at(index));
    } else {
        StreamConfig stream = StreamConfig::createDefault();
        // A new stream is runnable immediately rather than failing on "no sink".
        SinkConfig sink;
        sink.kind = SinkKind::TsMulticast;
        sink.ts = m_config.suggestMulticastEndpoint();
        stream.sinks.append(sink);
        m_draft->load(stream);
    }

    recomputeDraftProblems();
    Q_EMIT draftProblemsChanged();
}

bool BridgeController::commitEdit()
{
    StreamConfig edited = m_draft->toConfig();
    if (edited.id.isEmpty()) {
        edited.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    const QStringList problems = m_config.validateStream(edited, edited.id);
    if (!problems.isEmpty()) {
        m_draftProblems = problems;
        Q_EMIT draftProblemsChanged();
        setStatusMessage(problems.first());
        return false;
    }

    const int index = m_config.indexOfStream(edited.id);
    const bool wasRunning = m_engine->isStreamRunning(edited.id);

    if (index >= 0) {
        m_engine->stopStream(edited.id);
        m_config.streams[index] = edited;
    } else {
        m_config.streams.append(edited);
    }

    refreshModel();
    setDirty(true);
    Q_EMIT configChanged();

    if (wasRunning && edited.enabled) {
        m_engine->setConfig(m_config);
        m_engine->startStream(edited.id);
        Q_EMIT runningChanged();
    }

    setStatusMessage(u"Saved stream \"%1\""_s.arg(edited.name));
    return true;
}

void BridgeController::cancelEdit()
{
    m_draft->load(StreamConfig::createDefault());
    m_draftProblems.clear();
    Q_EMIT draftProblemsChanged();
}

void BridgeController::suggestMulticastFor(int sinkRow)
{
    SinkListModel *sinks = m_draft->sinks();
    if (sinkRow < 0 || sinkRow >= sinks->rowCount()) {
        return;
    }

    // Excludes the stream being edited so it can reuse its own current endpoint.
    BridgeConfig scratch = m_config;
    const int index = scratch.indexOfStream(m_draft->streamId());
    if (index >= 0) {
        scratch.streams.removeAt(index);
    }
    scratch.streams.append(m_draft->toConfig());

    const TsMulticastSinkConfig suggestion = scratch.suggestMulticastEndpoint();
    const QModelIndex row = sinks->index(sinkRow);
    sinks->setData(row, suggestion.groupAddress, SinkListModel::GroupAddressRole);
    sinks->setData(row, int(suggestion.port), SinkListModel::PortRole);
}

void BridgeController::recomputeDraftProblems()
{
    const StreamConfig candidate = m_draft->toConfig();
    QStringList problems = m_config.validateStream(candidate, candidate.id);
    if (candidate.name.trimmed().isEmpty()) {
        problems.prepend(u"The stream needs a name."_s);
    }

    if (m_draftProblems == problems) {
        return;
    }
    m_draftProblems = problems;
    Q_EMIT draftProblemsChanged();
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

QString BridgeController::tsAddressFor(const QString &streamId) const
{
    const int index = m_config.indexOfStream(streamId);
    if (index < 0) {
        return {};
    }

    for (const SinkConfig &sink : m_config.streams.at(index).sinks) {
        if (sink.kind != SinkKind::TsMulticast || !sink.enabled) {
            continue;
        }
        return u"udp://%1:%2"_s.arg(sink.ts.groupAddress).arg(sink.ts.port);
    }
    return {};
}

QStringList BridgeController::validationProblems() const
{
    return m_config.validate();
}

bool BridgeController::streamAutoStart(const QString &streamId) const
{
    KConfigGroup group(CBridgeSettings::self()->config(), u"Streams"_s);
    return group.readEntry(streamId, false);
}

QVariantList BridgeController::streamAutoStarts() const
{
    QVariantList rows;
    for (const StreamConfig &stream : m_config.streams) {
        rows.append(QVariantMap {
            { u"id"_s, stream.id },
            { u"name"_s, stream.name },
            { u"autoStart"_s, streamAutoStart(stream.id) },
        });
    }
    return rows;
}

void BridgeController::saveStartupSettings(const QString &configPath, bool autoLoadLastConfig,
                                           bool startOnLoad, const QVariantList &streamRows)
{
    CBridgeSettings::setConfigPath(configPath);
    CBridgeSettings::setAutoLoadLastConfig(autoLoadLastConfig);
    CBridgeSettings::setStartOnLoad(startOnLoad);

    KConfigGroup group(CBridgeSettings::self()->config(), u"Streams"_s);
    QStringList current;
    for (const QVariant &variant : streamRows) {
        const auto row = variant.toMap();
        const QString id = row.value(u"id"_s).toString();
        if (id.isEmpty()) {
            continue;
        }
        group.writeEntry(id, row.value(u"autoStart"_s).toBool());
        current.append(id);
    }

    // Drop flags for streams that no longer exist in the document.
    for (const QString &key : group.keyList()) {
        if (!current.contains(key)) {
            group.deleteEntry(key);
        }
    }

    CBridgeSettings::self()->save();
}

} // namespace CBridge
