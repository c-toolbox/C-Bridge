/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "core/bridgetypes.h"
#include "webrtc/linkheaderparser.h"

#include <QObject>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace rtc {
class PeerConnection;
class Track;
}

namespace CBridge {

class WhepClient;

/// Pulls one WHEP stream and hands out elementary media.
///
/// Media is delivered on libdatachannel's own threads through plain std::function
/// callbacks rather than Qt signals: the hot path must not queue through the GUI
/// event loop. State changes do use signals and are safe to bind to the UI.
class WebRtcSource : public QObject
{
    Q_OBJECT

public:
    /// data points at an Annex-B access unit (video) or an Opus packet (audio).
    /// It is only valid for the duration of the call.
    using MediaFrameCallback =
        std::function<void(const std::uint8_t *data, std::size_t size, quint32 rtpTimestamp)>;

    explicit WebRtcSource(QObject *parent = nullptr);
    ~WebRtcSource() override;

    void setConfig(const StreamConfig &config);
    void setPassword(const QString &password);

    void setVideoCallback(MediaFrameCallback callback);
    void setAudioCallback(MediaFrameCallback callback);

    void start();
    void stop();

    StreamState state() const;
    VideoCodec negotiatedVideoCodec() const;

    /// Asks the sender for an IDR. MediaMTX does not send one on connect, so this
    /// is called on track open and whenever a consumer reports it is starved.
    void requestKeyframe();

Q_SIGNALS:
    void stateChanged(CBridge::StreamState state);
    void videoCodecNegotiated(CBridge::VideoCodec codec);
    void errorOccurred(const QString &message);

private:
    void beginNegotiation(const QList<IceServerSpec> &iceServers);
    void onLocalDescriptionReady();
    void onAnswer(const QString &sdpAnswer);
    void setState(StreamState state);
    void teardownPeer();

    StreamConfig m_config;
    QString m_password;

    WhepClient *m_whep = nullptr;
    std::shared_ptr<rtc::PeerConnection> m_peer;
    std::shared_ptr<rtc::Track> m_videoTrack;
    std::shared_ptr<rtc::Track> m_audioTrack;

    MediaFrameCallback m_onVideo;
    MediaFrameCallback m_onAudio;

    std::atomic<StreamState> m_state { StreamState::Idle };
    std::atomic<VideoCodec> m_videoCodec { VideoCodec::Unknown };
    bool m_offerSent = false;
};

} // namespace CBridge
