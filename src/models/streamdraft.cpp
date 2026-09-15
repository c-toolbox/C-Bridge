/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "models/streamdraft.h"

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

StreamDraft::StreamDraft(QObject *parent)
    : QObject(parent)
    , m_sinks(new SinkListModel(this))
{
    connect(m_sinks, &SinkListModel::sinksChanged, this, &StreamDraft::changed);
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
    m_sinks->setSinks(config.sinks);

    Q_EMIT nameChanged();
    Q_EMIT enabledChanged();
    Q_EMIT whepUrlChanged();
    Q_EMIT usernameChanged();
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
