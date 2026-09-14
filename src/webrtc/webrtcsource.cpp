#include "webrtc/webrtcsource.h"

#include "webrtc/whepclient.h"

#include <QRegularExpression>
#include <QStringList>

#include <rtc/rtc.hpp>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

constexpr int kVideoPayloadTypeH264 = 106;
constexpr int kVideoPayloadTypeH265 = 103;
constexpr int kAudioPayloadTypeOpus = 111;

/// MediaMTX answers with exactly one video codec; find out which so the right
/// depacketizer can be installed before media starts flowing.
VideoCodec videoCodecFromAnswer(const QString &sdp)
{
    static const QRegularExpression rtpMap(
        uR"(^a=rtpmap:(\d+)\s+([A-Za-z0-9]+)/(\d+))"_s,
        QRegularExpression::MultilineOption);

    auto it = rtpMap.globalMatch(sdp);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString encoding = match.captured(2).toUpper();
        if (encoding == u"H264"_s) {
            return VideoCodec::H264;
        }
        if (encoding == u"H265"_s || encoding == u"HEVC"_s) {
            return VideoCodec::H265;
        }
    }
    return VideoCodec::Unknown;
}

} // namespace

WebRtcSource::WebRtcSource(QObject *parent)
    : QObject(parent)
{
}

WebRtcSource::~WebRtcSource()
{
    stop();
}

void WebRtcSource::setConfig(const StreamConfig &config)
{
    m_config = config;
}

void WebRtcSource::setPassword(const QString &password)
{
    m_password = password;
}

void WebRtcSource::setVideoCallback(MediaFrameCallback callback)
{
    m_onVideo = std::move(callback);
}

void WebRtcSource::setAudioCallback(MediaFrameCallback callback)
{
    m_onAudio = std::move(callback);
}

StreamState WebRtcSource::state() const
{
    return m_state.load(std::memory_order_relaxed);
}

VideoCodec WebRtcSource::negotiatedVideoCodec() const
{
    return m_videoCodec.load(std::memory_order_relaxed);
}

void WebRtcSource::setState(StreamState state)
{
    const StreamState previous = m_state.exchange(state, std::memory_order_relaxed);
    if (previous != state) {
        Q_EMIT stateChanged(state);
    }
}

void WebRtcSource::start()
{
    stop();

    m_offerSent = false;
    m_videoCodec.store(VideoCodec::Unknown, std::memory_order_relaxed);
    setState(StreamState::Connecting);

    m_whep = new WhepClient(this);
    m_whep->setEndpoint(m_config.whepUrl);
    m_whep->setCredentials(m_config.username, m_password);

    connect(m_whep, &WhepClient::iceServersReady, this, &WebRtcSource::beginNegotiation);
    connect(m_whep, &WhepClient::answerReceived, this, &WebRtcSource::onAnswer);
    connect(m_whep, &WhepClient::failed, this, [this](const QString &reason) {
        Q_EMIT errorOccurred(reason);
        setState(StreamState::Failed);
    });

    m_whep->requestIceServers();
}

void WebRtcSource::beginNegotiation(const QList<IceServerSpec> &iceServers)
{
    rtc::Configuration configuration;
    for (const IceServerSpec &server : iceServers) {
        try {
            rtc::IceServer entry(server.url.toStdString());
            if (!server.username.isEmpty()) {
                entry.username = server.username.toStdString();
                entry.password = server.credential.toStdString();
            }
            configuration.iceServers.push_back(std::move(entry));
        } catch (const std::exception &error) {
            qWarning("Ignoring unusable ICE server %s: %s",
                     qUtf8Printable(server.url), error.what());
        }
    }

    configuration.disableAutoNegotiation = true;

    try {
        m_peer = std::make_shared<rtc::PeerConnection>(configuration);
    } catch (const std::exception &error) {
        Q_EMIT errorOccurred(u"Could not create the peer connection: %1"_s
                                 .arg(QString::fromUtf8(error.what())));
        setState(StreamState::Failed);
        return;
    }

    m_peer->onStateChange([this](rtc::PeerConnection::State state) {
        switch (state) {
        case rtc::PeerConnection::State::Connected:
            setState(StreamState::Running);
            break;
        case rtc::PeerConnection::State::Disconnected:
        case rtc::PeerConnection::State::Failed:
            setState(StreamState::Failed);
            break;
        case rtc::PeerConnection::State::Closed:
            setState(StreamState::Idle);
            break;
        default:
            break;
        }
    });

    m_peer->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            QMetaObject::invokeMethod(this, [this] { onLocalDescriptionReady(); },
                                      Qt::QueuedConnection);
        }
    });

    rtc::Description::Video video("video", rtc::Description::Direction::RecvOnly);
    for (VideoCodec codec : m_config.preferredCodecs) {
        switch (codec) {
        case VideoCodec::H264:
            video.addH264Codec(kVideoPayloadTypeH264);
            break;
        case VideoCodec::H265:
            video.addH265Codec(kVideoPayloadTypeH265);
            break;
        case VideoCodec::Unknown:
            break;
        }
    }
    m_videoTrack = m_peer->addTrack(video);

    if (m_config.audioEnabled) {
        rtc::Description::Audio audio("audio", rtc::Description::Direction::RecvOnly);
        audio.addOpusCodec(kAudioPayloadTypeOpus);
        m_audioTrack = m_peer->addTrack(audio);
    }

    try {
        m_peer->setLocalDescription(rtc::Description::Type::Offer);
    } catch (const std::exception &error) {
        Q_EMIT errorOccurred(u"Could not create the SDP offer: %1"_s
                                 .arg(QString::fromUtf8(error.what())));
        setState(StreamState::Failed);
    }
}

void WebRtcSource::onLocalDescriptionReady()
{
    if (m_offerSent || !m_peer) {
        return;
    }
    const auto description = m_peer->localDescription();
    if (!description) {
        return;
    }

    m_offerSent = true;
    m_whep->sendOffer(QString::fromStdString(std::string(description.value())));
}

void WebRtcSource::onAnswer(const QString &sdpAnswer)
{
    if (!m_peer) {
        return;
    }

    const VideoCodec codec = videoCodecFromAnswer(sdpAnswer);
    m_videoCodec.store(codec, std::memory_order_relaxed);
    if (codec == VideoCodec::Unknown) {
        Q_EMIT errorOccurred(u"The WHEP answer contains no supported video codec"_s);
        setState(StreamState::Failed);
        return;
    }
    Q_EMIT videoCodecNegotiated(codec);

    // Handlers must be in place before setRemoteDescription starts the media flow.
    if (m_videoTrack) {
        std::shared_ptr<rtc::MediaHandler> depacketizer;
        if (codec == VideoCodec::H265) {
            depacketizer = std::make_shared<rtc::H265RtpDepacketizer>(
                rtc::NalUnit::Separator::LongStartSequence);
        } else {
            depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(
                rtc::NalUnit::Separator::LongStartSequence);
        }

        m_videoTrack->setMediaHandler(depacketizer);
        m_videoTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

        m_videoTrack->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
            if (m_onVideo && !data.empty()) {
                m_onVideo(reinterpret_cast<const std::uint8_t *>(data.data()), data.size(),
                          info.timestamp);
            }
        });

        m_videoTrack->onOpen([this] { requestKeyframe(); });
    }

    if (m_audioTrack) {
        // libdatachannel has no Opus-specific depacketizer; the generic one strips
        // the RTP header and yields the raw Opus payload, which is what we forward.
        m_audioTrack->setMediaHandler(std::make_shared<rtc::RtpDepacketizer>());
        m_audioTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

        m_audioTrack->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
            if (m_onAudio && !data.empty()) {
                m_onAudio(reinterpret_cast<const std::uint8_t *>(data.data()), data.size(),
                          info.timestamp);
            }
        });
    }

    try {
        m_peer->setRemoteDescription(
            rtc::Description(sdpAnswer.toStdString(), rtc::Description::Type::Answer));
    } catch (const std::exception &error) {
        Q_EMIT errorOccurred(u"Could not apply the SDP answer: %1"_s
                                 .arg(QString::fromUtf8(error.what())));
        setState(StreamState::Failed);
    }
}

void WebRtcSource::requestKeyframe()
{
    if (m_videoTrack && m_videoTrack->isOpen()) {
        m_videoTrack->requestKeyframe();
    }
}

void WebRtcSource::teardownPeer()
{
    if (m_videoTrack) {
        m_videoTrack->onFrame(nullptr);
        m_videoTrack.reset();
    }
    if (m_audioTrack) {
        m_audioTrack->onFrame(nullptr);
        m_audioTrack.reset();
    }
    if (m_peer) {
        try {
            m_peer->close();
        } catch (const std::exception &error) {
            qWarning("Error while closing the peer connection: %s", error.what());
        }
        m_peer.reset();
    }
}

void WebRtcSource::stop()
{
    if (!m_whep && !m_peer) {
        return;
    }

    setState(StreamState::Stopping);

    if (m_whep) {
        m_whep->deleteSession();
        m_whep->deleteLater();
        m_whep = nullptr;
    }

    teardownPeer();
    setState(StreamState::Idle);
}

} // namespace CBridge
