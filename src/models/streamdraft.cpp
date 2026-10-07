/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "models/streamdraft.h"
#include "ytdlp/compatibleformats.h"
#include "cbridgesettings.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

StreamDraft::StreamDraft(QObject *parent)
    : QObject(parent)
    , m_sinks(new SinkListModel(this))
{
    connect(m_sinks, &SinkListModel::sinksChanged, this, &StreamDraft::changed);
    m_youtubeProbeDelay.setSingleShot(true);
    m_youtubeProbeDelay.setInterval(600);
    connect(&m_youtubeProbeDelay, &QTimer::timeout, this, &StreamDraft::refreshYoutubeFormats);
}

StreamDraft::~StreamDraft()
{
    cancelYoutubeProbe();
}

void StreamDraft::cancelYoutubeProbe()
{
    m_youtubeProbeDelay.stop();
    if (m_youtubeProbe) {
        m_youtubeProbe->disconnect(this);
        m_youtubeProbe->kill();
        m_youtubeProbe->deleteLater();
        m_youtubeProbe = nullptr;
    }
    m_youtubeFormatsBusy = false;
}

void StreamDraft::scheduleYoutubeProbe()
{
    cancelYoutubeProbe();
    m_youtubeFormats.clear();
    m_youtubeResolutions.clear();
    m_youtubeAudioOptions.clear();
    m_youtubeResolutionIndex = m_youtubeAudioIndex = -1;
    m_youtubeFormatsError.clear();
    if (m_config.sourceKind == SourceKind::Youtube
        && (m_config.youtube.url.scheme() == u"https"_s || m_config.youtube.url.scheme() == u"http"_s)
        && !m_config.youtube.url.host().isEmpty()) {
        m_youtubeFormatsBusy = true;
        m_youtubeProbeDelay.start();
    }
    Q_EMIT youtubeFormatsChanged();
}

void StreamDraft::refreshYoutubeFormats()
{
    cancelYoutubeProbe();
    if (m_config.sourceKind != SourceKind::Youtube || m_config.youtube.url.host().isEmpty()) return;
    QString program = m_config.youtube.ytDlpPath.trimmed();
    if (program.isEmpty()) {
        program = CBridgeSettings::self()->ytdlpPath().trimmed();
        if (!QFileInfo::exists(program)) program = QStandardPaths::findExecutable(u"yt-dlp"_s);
        if (program.isEmpty() && QFileInfo::exists(u"D:/FFmpeg/yt-dlp.exe"_s))
            program = u"D:/FFmpeg/yt-dlp.exe"_s;
    }
    m_youtubeFormatsError.clear();
    m_youtubeFormatsBusy = true;
    Q_EMIT youtubeFormatsChanged();
    auto *probe = new QProcess(this);
    m_youtubeProbe = probe;
    auto fail = [this, probe](const QString &message) {
        if (m_youtubeProbe != probe) return;
        cancelYoutubeProbe();
        m_youtubeFormats.clear();
        m_youtubeResolutions.clear();
        m_youtubeAudioOptions.clear();
        m_youtubeResolutionIndex = m_youtubeAudioIndex = -1;
        m_youtubeFormatsError = message;
        Q_EMIT youtubeFormatsChanged();
    };
    connect(probe, &QProcess::errorOccurred, this, [fail, probe](QProcess::ProcessError) {
        fail(tr("Could not query yt-dlp: %1").arg(probe->errorString()));
    });
    connect(probe, &QProcess::readyReadStandardOutput, this, [probe, fail]() {
        QByteArray data = probe->property("metadata").toByteArray() + probe->readAllStandardOutput();
        if (data.size() > 8 * 1024 * 1024) {
            fail(tr("yt-dlp returned too much metadata."));
            return;
        }
        probe->setProperty("metadata", data);
    });
    connect(probe, &QProcess::readyReadStandardError, this, [probe]() {
        probe->setProperty("errors", (probe->property("errors").toByteArray()
                                     + probe->readAllStandardError()).right(4096));
    });
    connect(probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, probe, fail](int code, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || code != 0) {
            fail(tr("yt-dlp could not list formats: %1").arg(QString::fromUtf8(
                probe->property("errors").toByteArray() + probe->readAllStandardError()).trimmed()));
            return;
        }
        const auto data = probe->property("metadata").toByteArray() + probe->readAllStandardOutput();
        const auto doc = QJsonDocument::fromJson(data);
        if (data.size() > 8 * 1024 * 1024 || !doc.isObject()) {
            fail(tr("yt-dlp returned invalid video metadata."));
            return;
        }
        m_youtubeFormats = compatibleYoutubeFormats(doc.object().value(u"formats"_s).toArray());
        m_youtubeResolutions.clear();
        for (const auto &value : m_youtubeFormats) {
            const auto label = value.toMap().value(u"resolution"_s).toString();
            if (!m_youtubeResolutions.contains(label)) m_youtubeResolutions.append(label);
        }
        m_youtubeFormatsBusy = false;
        m_youtubeProbe = nullptr;
        probe->deleteLater();
        if (m_youtubeFormats.isEmpty())
            m_youtubeFormatsError = tr("No supported video formats are available for this URL.");
        syncYoutubeSelection();
    });
    QTimer::singleShot(30000, probe, [this, probe, fail]() {
        if (m_youtubeProbe == probe) fail(tr("yt-dlp format query timed out. Try again."));
    });
    QStringList args = QProcess::splitCommand(m_config.youtube.extraArgs);
    // Keep this a metadata request even when extra arguments include output/format flags.
    args << u"--ignore-config"_s << u"--quiet"_s << u"--no-warnings"_s
         << u"--no-playlist"_s << u"--dump-single-json"_s << u"--skip-download"_s
         << u"-f"_s << u"all"_s << u"--socket-timeout"_s << u"10"_s
         << u"--"_s << m_config.youtube.url.toString();
    probe->start(program, args);
}

void StreamDraft::updateYoutubeAudioOptions()
{
    m_youtubeAudioOptions.clear();
    m_youtubeAudioIndex = -1;
    if (m_youtubeResolutionIndex < 0 || m_youtubeResolutionIndex >= m_youtubeResolutions.size()) return;
    const auto resolution = m_youtubeResolutions.at(m_youtubeResolutionIndex);
    for (const auto &value : m_youtubeFormats) {
        const auto format = value.toMap();
        if (format.value(u"resolution"_s).toString() == resolution) {
            if (format.value(u"id"_s).toString() == formatSelector())
                m_youtubeAudioIndex = int(m_youtubeAudioOptions.size());
            m_youtubeAudioOptions.append(format);
        }
    }
}

void StreamDraft::syncYoutubeSelection()
{
    m_youtubeResolutionIndex = -1;
    for (const auto &value : m_youtubeFormats) {
        const auto format = value.toMap();
        if (format.value(u"id"_s).toString() == formatSelector()) {
            m_youtubeResolutionIndex = int(m_youtubeResolutions.indexOf(format.value(u"resolution"_s).toString()));
            break;
        }
    }
    updateYoutubeAudioOptions();
    Q_EMIT youtubeFormatsChanged();
}

void StreamDraft::selectYoutubeResolution(int index)
{
    if (index < 0 || index >= m_youtubeResolutions.size()) return;
    // Keep an independently chosen audio track when changing video resolution/codec.
    const auto audioId = m_youtubeAudioIndex >= 0 && m_youtubeAudioIndex < m_youtubeAudioOptions.size()
        ? m_youtubeAudioOptions.at(m_youtubeAudioIndex).toMap().value(u"audioId"_s).toString() : QString {};
    m_youtubeResolutionIndex = index;
    updateYoutubeAudioOptions();
    if (!audioId.isEmpty()) {
        for (int i = 0; i < m_youtubeAudioOptions.size(); ++i) {
            if (m_youtubeAudioOptions.at(i).toMap().value(u"audioId"_s).toString() == audioId) {
                selectYoutubeAudio(i);
                return;
            }
        }
    }
    selectYoutubeAudio(0);
}

void StreamDraft::selectYoutubeAudio(int index)
{
    if (index < 0 || index >= m_youtubeAudioOptions.size()) return;
    const auto format = m_youtubeAudioOptions.at(index).toMap();
    setFormatSelector(format.value(u"id"_s).toString());
    setAudioEnabled(format.value(u"hasAudio"_s).toBool());
}

QStringList StreamDraft::preferredCodecs() const
{
    QStringList names;
    names.reserve(int(m_config.preferredCodecs.size()));
    for (VideoCodec codec : m_config.preferredCodecs) {
        names.append(CBridge::toString(codec));
    }
    return names;
}

void StreamDraft::setName(const QString &name)
{
    if (m_config.name == name) {
        return;
    }
    m_config.name = name;
    Q_EMIT nameChanged();
    Q_EMIT changed();
}

void StreamDraft::setEnabled(bool enabled)
{
    if (m_config.enabled == enabled) {
        return;
    }
    m_config.enabled = enabled;
    Q_EMIT enabledChanged();
    Q_EMIT changed();
}

void StreamDraft::setSourceKind(const QString &kind)
{
    const SourceKind parsed = sourceKindFromString(kind);
    if (m_config.sourceKind == parsed) {
        return;
    }
    m_config.sourceKind = parsed;
    scheduleYoutubeProbe();
    Q_EMIT sourceKindChanged();
    Q_EMIT changed();
}

void StreamDraft::setWhepUrl(const QString &url)
{
    const QUrl parsed(url);
    if (m_config.whepUrl == parsed) {
        return;
    }
    m_config.whepUrl = parsed;
    Q_EMIT whepUrlChanged();
    Q_EMIT changed();
}

void StreamDraft::setUsername(const QString &username)
{
    if (m_config.username == username) {
        return;
    }
    m_config.username = username;
    Q_EMIT usernameChanged();
    Q_EMIT changed();
}

void StreamDraft::setSrtCaller(bool caller)
{
    const SrtSourceConfig::Mode mode = caller ? SrtSourceConfig::Mode::Caller : SrtSourceConfig::Mode::Listener;
    if (m_config.srt.mode == mode) {
        return;
    }
    m_config.srt.mode = mode;
    Q_EMIT srtModeChanged();
    Q_EMIT changed();
}

void StreamDraft::setSrtHost(const QString &host)
{
    if (m_config.srt.host == host) {
        return;
    }
    m_config.srt.host = host.trimmed();
    Q_EMIT srtHostChanged();
    Q_EMIT changed();
}

void StreamDraft::setSrtPort(int port)
{
    const int clamped = qBound(1, port, 65535);
    if (int(m_config.srt.port) == clamped) {
        return;
    }
    m_config.srt.port = quint16(clamped);
    Q_EMIT srtPortChanged();
    Q_EMIT changed();
}

void StreamDraft::setSrtPassphrase(const QString &passphrase)
{
    if (m_config.srt.passphrase == passphrase) {
        return;
    }
    m_config.srt.passphrase = passphrase;
    Q_EMIT srtPassphraseChanged();
    Q_EMIT changed();
}

void StreamDraft::setSrtLatencyMs(int ms)
{
    const int clamped = qBound(0, ms, 60000);
    if (m_config.srt.latencyMs == clamped) {
        return;
    }
    m_config.srt.latencyMs = clamped;
    Q_EMIT srtLatencyMsChanged();
    Q_EMIT changed();
}

void StreamDraft::setSrtStreamId(const QString &streamId)
{
    const QString trimmed = streamId.trimmed();
    if (m_config.srt.streamId == trimmed) {
        return;
    }
    m_config.srt.streamId = trimmed;
    Q_EMIT srtStreamIdChanged();
    Q_EMIT changed();
}

void StreamDraft::setYoutubeUrl(const QString &url)
{
    const QUrl parsed(url.trimmed());
    if (m_config.youtube.url == parsed) {
        return;
    }
    m_config.youtube.url = parsed;
    setFormatSelector(YouTubeSourceConfig {}.formatSelector);
    scheduleYoutubeProbe();
    Q_EMIT youtubeUrlChanged();
    Q_EMIT changed();
}

void StreamDraft::setFormatSelector(const QString &selector)
{
    const QString trimmed = selector.trimmed();
    if (m_config.youtube.formatSelector == trimmed) {
        return;
    }
    m_config.youtube.formatSelector = trimmed;
    syncYoutubeSelection();
    Q_EMIT formatSelectorChanged();
    Q_EMIT changed();
}

void StreamDraft::setExtraArgs(const QString &args)
{
    const QString trimmed = args.trimmed();
    if (m_config.youtube.extraArgs == trimmed) {
        return;
    }
    m_config.youtube.extraArgs = trimmed;
    scheduleYoutubeProbe();
    Q_EMIT extraArgsChanged();
    Q_EMIT changed();
}

void StreamDraft::setAudioBitrateKbps(int kbps)
{
    const int clamped = qBound(32, kbps, 510);
    if (m_config.youtube.audioBitrateKbps == clamped) {
        return;
    }
    m_config.youtube.audioBitrateKbps = clamped;
    Q_EMIT audioBitrateKbpsChanged();
    Q_EMIT changed();
}

void StreamDraft::setConcurrentFragments(int fragments)
{
    const int clamped = qBound(1, fragments, 16);
    if (m_config.youtube.concurrentFragments == clamped) {
        return;
    }
    m_config.youtube.concurrentFragments = clamped;
    Q_EMIT concurrentFragmentsChanged();
    Q_EMIT changed();
}

void StreamDraft::setDirectUrlMode(const QString &mode)
{
    const QString normalized = (mode == u"off"_s || mode == u"force"_s) ? mode : u"auto"_s;
    if (m_config.youtube.directUrlMode == normalized) {
        return;
    }
    m_config.youtube.directUrlMode = normalized;
    Q_EMIT directUrlModeChanged();
    Q_EMIT changed();
}

void StreamDraft::setAudioEnabled(bool enabled)
{
    if (m_config.audioEnabled == enabled) {
        return;
    }
    m_config.audioEnabled = enabled;
    Q_EMIT audioEnabledChanged();
    Q_EMIT changed();
}

void StreamDraft::setPreferredCodecs(const QStringList &codecs)
{
    QList<VideoCodec> resolved;
    for (const QString &name : codecs) {
        const VideoCodec codec = videoCodecFromString(name);
        if (codec != VideoCodec::Unknown && !resolved.contains(codec)) {
            resolved.append(codec);
        }
    }
    if (resolved.isEmpty()) {
        resolved = { VideoCodec::H264, VideoCodec::H265 };
    }
    if (m_config.preferredCodecs == resolved) {
        return;
    }
    m_config.preferredCodecs = resolved;
    Q_EMIT preferredCodecsChanged();
    Q_EMIT changed();
}

void StreamDraft::setReconnectInitialMs(int ms)
{
    const int clamped = qBound(100, ms, 60000);
    if (m_config.reconnectInitialMs == clamped) {
        return;
    }
    m_config.reconnectInitialMs = clamped;
    Q_EMIT reconnectInitialMsChanged();
    Q_EMIT changed();
}

void StreamDraft::setReconnectMaxMs(int ms)
{
    const int clamped = qBound(m_config.reconnectInitialMs, ms, 300000);
    if (m_config.reconnectMaxMs == clamped) {
        return;
    }
    m_config.reconnectMaxMs = clamped;
    Q_EMIT reconnectMaxMsChanged();
    Q_EMIT changed();
}

void StreamDraft::load(const StreamConfig &config)
{
    m_config = config;
    scheduleYoutubeProbe();
    m_sinks->setSinks(config.sinks);

    Q_EMIT nameChanged();
    Q_EMIT enabledChanged();
    Q_EMIT sourceKindChanged();
    Q_EMIT whepUrlChanged();
    Q_EMIT usernameChanged();
    Q_EMIT srtModeChanged();
    Q_EMIT srtHostChanged();
    Q_EMIT srtPortChanged();
    Q_EMIT srtPassphraseChanged();
    Q_EMIT srtLatencyMsChanged();
    Q_EMIT srtStreamIdChanged();
    Q_EMIT youtubeUrlChanged();
    Q_EMIT formatSelectorChanged();
    Q_EMIT extraArgsChanged();
    Q_EMIT audioBitrateKbpsChanged();
    Q_EMIT audioEnabledChanged();
    Q_EMIT preferredCodecsChanged();
    Q_EMIT reconnectInitialMsChanged();
    Q_EMIT reconnectMaxMsChanged();
    Q_EMIT changed();
}

StreamConfig StreamDraft::toConfig() const
{
    StreamConfig config = m_config;
    config.sinks = m_sinks->sinks();
    return config;
}

} // namespace CBridge
