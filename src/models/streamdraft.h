/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "models/sinklistmodel.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QProcess>
#include <QTimer>

namespace CBridge {

/// A detached, editable copy of a StreamConfig. Cancel is real because nothing
/// here is written back until the controller commits it.
class StreamDraft : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString streamId READ streamId NOTIFY changed)
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QString sourceKind READ sourceKind WRITE setSourceKind NOTIFY sourceKindChanged)
    Q_PROPERTY(QString whepUrl READ whepUrl WRITE setWhepUrl NOTIFY whepUrlChanged)
    Q_PROPERTY(QString username READ username WRITE setUsername NOTIFY usernameChanged)
    Q_PROPERTY(bool srtIsCaller READ isSrtCaller WRITE setSrtCaller NOTIFY srtModeChanged)
    Q_PROPERTY(QString srtHost READ srtHost WRITE setSrtHost NOTIFY srtHostChanged)
    Q_PROPERTY(int srtPort READ srtPort WRITE setSrtPort NOTIFY srtPortChanged)
    Q_PROPERTY(QString srtPassphrase READ srtPassphrase WRITE setSrtPassphrase NOTIFY srtPassphraseChanged)
    Q_PROPERTY(int srtLatencyMs READ srtLatencyMs WRITE setSrtLatencyMs NOTIFY srtLatencyMsChanged)
    Q_PROPERTY(QString srtStreamId READ srtStreamId WRITE setSrtStreamId NOTIFY srtStreamIdChanged)
    Q_PROPERTY(QString youtubeUrl READ youtubeUrl WRITE setYoutubeUrl NOTIFY youtubeUrlChanged)
    Q_PROPERTY(QString formatSelector READ formatSelector WRITE setFormatSelector NOTIFY formatSelectorChanged)
    Q_PROPERTY(QString extraArgs READ extraArgs WRITE setExtraArgs NOTIFY extraArgsChanged)
    Q_PROPERTY(int audioBitrateKbps READ audioBitrateKbps WRITE setAudioBitrateKbps NOTIFY audioBitrateKbpsChanged)
    Q_PROPERTY(int concurrentFragments READ concurrentFragments WRITE setConcurrentFragments NOTIFY concurrentFragmentsChanged)
    Q_PROPERTY(QString directUrlMode READ directUrlMode WRITE setDirectUrlMode NOTIFY directUrlModeChanged)
    Q_PROPERTY(QStringList youtubeResolutions READ youtubeResolutions NOTIFY youtubeFormatsChanged)
    Q_PROPERTY(QVariantList youtubeAudioOptions READ youtubeAudioOptions NOTIFY youtubeFormatsChanged)
    Q_PROPERTY(int youtubeResolutionIndex READ youtubeResolutionIndex NOTIFY youtubeFormatsChanged)
    Q_PROPERTY(int youtubeAudioIndex READ youtubeAudioIndex NOTIFY youtubeFormatsChanged)
    Q_PROPERTY(bool youtubeFormatsBusy READ youtubeFormatsBusy NOTIFY youtubeFormatsChanged)
    Q_PROPERTY(QString youtubeFormatsError READ youtubeFormatsError NOTIFY youtubeFormatsChanged)
    Q_PROPERTY(bool audioEnabled READ isAudioEnabled WRITE setAudioEnabled NOTIFY audioEnabledChanged)
    Q_PROPERTY(QStringList preferredCodecs READ preferredCodecs WRITE setPreferredCodecs NOTIFY preferredCodecsChanged)
    Q_PROPERTY(int reconnectInitialMs READ reconnectInitialMs WRITE setReconnectInitialMs NOTIFY reconnectInitialMsChanged)
    Q_PROPERTY(int reconnectMaxMs READ reconnectMaxMs WRITE setReconnectMaxMs NOTIFY reconnectMaxMsChanged)
    Q_PROPERTY(CBridge::SinkListModel *sinks READ sinks CONSTANT)

public:
    explicit StreamDraft(QObject *parent = nullptr);
    ~StreamDraft() override;

    QString streamId() const { return m_config.id; }
    QString name() const { return m_config.name; }
    bool isEnabled() const { return m_config.enabled; }
    QString sourceKind() const { return toString(m_config.sourceKind); }
    QString whepUrl() const { return m_config.whepUrl.toString(); }
    QString username() const { return m_config.username; }
    bool isSrtCaller() const { return m_config.srt.mode == SrtSourceConfig::Mode::Caller; }
    QString srtHost() const { return m_config.srt.host; }
    int srtPort() const { return int(m_config.srt.port); }
    QString srtPassphrase() const { return m_config.srt.passphrase; }
    int srtLatencyMs() const { return m_config.srt.latencyMs; }
    QString srtStreamId() const { return m_config.srt.streamId; }
    QString youtubeUrl() const { return m_config.youtube.url.toString(); }
    QString formatSelector() const { return m_config.youtube.formatSelector; }
    QString extraArgs() const { return m_config.youtube.extraArgs; }
    int audioBitrateKbps() const { return m_config.youtube.audioBitrateKbps; }
    int concurrentFragments() const { return m_config.youtube.concurrentFragments; }
    QString directUrlMode() const { return m_config.youtube.directUrlMode; }
    QStringList youtubeResolutions() const { return m_youtubeResolutions; }
    QVariantList youtubeAudioOptions() const { return m_youtubeAudioOptions; }
    int youtubeResolutionIndex() const { return m_youtubeResolutionIndex; }
    int youtubeAudioIndex() const { return m_youtubeAudioIndex; }
    bool youtubeFormatsBusy() const { return m_youtubeFormatsBusy; }
    QString youtubeFormatsError() const { return m_youtubeFormatsError; }
    Q_INVOKABLE void refreshYoutubeFormats();
    Q_INVOKABLE void selectYoutubeResolution(int index);
    Q_INVOKABLE void selectYoutubeAudio(int index);
    bool isAudioEnabled() const { return m_config.audioEnabled; }
    QStringList preferredCodecs() const;
    int reconnectInitialMs() const { return m_config.reconnectInitialMs; }
    int reconnectMaxMs() const { return m_config.reconnectMaxMs; }
    SinkListModel *sinks() const { return m_sinks; }

    void setName(const QString &name);
    void setEnabled(bool enabled);
    void setSourceKind(const QString &kind);
    void setWhepUrl(const QString &url);
    void setUsername(const QString &username);
    void setSrtCaller(bool caller);
    void setSrtHost(const QString &host);
    void setSrtPort(int port);
    void setSrtPassphrase(const QString &passphrase);
    void setSrtLatencyMs(int ms);
    void setSrtStreamId(const QString &streamId);
    void setYoutubeUrl(const QString &url);
    void setFormatSelector(const QString &selector);
    void setExtraArgs(const QString &args);
    void setAudioBitrateKbps(int kbps);
    void setConcurrentFragments(int fragments);
    void setDirectUrlMode(const QString &mode);
    void setAudioEnabled(bool enabled);
    void setPreferredCodecs(const QStringList &codecs);
    void setReconnectInitialMs(int ms);
    void setReconnectMaxMs(int ms);

    void load(const StreamConfig &config);
    StreamConfig toConfig() const;

Q_SIGNALS:
    void nameChanged();
    void enabledChanged();
    void sourceKindChanged();
    void whepUrlChanged();
    void usernameChanged();
    void srtModeChanged();
    void srtHostChanged();
    void srtPortChanged();
    void srtPassphraseChanged();
    void srtLatencyMsChanged();
    void srtStreamIdChanged();
    void youtubeUrlChanged();
    void formatSelectorChanged();
    void extraArgsChanged();
    void audioBitrateKbpsChanged();
    void concurrentFragmentsChanged();
    void directUrlModeChanged();
    void youtubeFormatsChanged();
    void audioEnabledChanged();
    void preferredCodecsChanged();
    void reconnectInitialMsChanged();
    void reconnectMaxMsChanged();

    /// Fired alongside every specific NOTIFY so validation recomputes once.
    void changed();

private:
    void scheduleYoutubeProbe();
    void cancelYoutubeProbe();
    void syncYoutubeSelection();
    void updateYoutubeAudioOptions();
    QTimer m_youtubeProbeDelay;
    QProcess *m_youtubeProbe = nullptr;
    QVariantList m_youtubeFormats;
    QStringList m_youtubeResolutions;
    QVariantList m_youtubeAudioOptions;
    int m_youtubeResolutionIndex = -1;
    int m_youtubeAudioIndex = -1;
    bool m_youtubeFormatsBusy = false;
    QString m_youtubeFormatsError;
    StreamConfig m_config;
    SinkListModel *m_sinks = nullptr;
};

} // namespace CBridge
