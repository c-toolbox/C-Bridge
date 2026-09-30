/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "core/bridgetypes.h"
#include "core/streamsource.h"
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
class WebRtcSource : public StreamSource
{
    Q_OBJECT

public:
    explicit WebRtcSource(QObject *parent = nullptr);
    ~WebRtcSource() override;

    void setConfig(const StreamConfig &config) override;
    void setPassword(const QString &password) override;

    /// Diagnostic hook (used by the smoke test): receives each raw audio RTP datagram, header
    /// included, before depacketization and padding stripping, so wire-level behaviour such as
    /// RFC 3550 padding can be verified. Not called when unset.
    using RawAudioPacketCallback = std::function<void(const std::uint8_t *data, std::size_t size)>;
    void setRawAudioPacketCallback(RawAudioPacketCallback callback);

    void start() override;
    void stop() override;

    VideoCodec negotiatedVideoCodec() const override;
    AudioCodec negotiatedAudioCodec() const override { return AudioCodec::Opus; }

    /// Asks the sender for an IDR. MediaMTX does not send one on connect, so this
    /// is called on track open and whenever a consumer reports it is starved.
    void requestKeyframe();

private:
    void beginNegotiation(const QList<IceServerSpec> &iceServers);
    void onLocalDescriptionReady();
    void onAnswer(const QString &sdpAnswer);
    void teardownPeer();

    StreamConfig m_config;
    QString m_password;

    WhepClient *m_whep = nullptr;
    std::shared_ptr<rtc::PeerConnection> m_peer;
    std::shared_ptr<rtc::Track> m_videoTrack;
    std::shared_ptr<rtc::Track> m_audioTrack;

    RawAudioPacketCallback m_onRawAudioPacket;

    std::atomic<VideoCodec> m_videoCodec { VideoCodec::Unknown };
    bool m_offerSent = false;
};

} // namespace CBridge
