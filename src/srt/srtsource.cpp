/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "srt/srtsource.h"

extern "C" {
#include <libavformat/avformat.h>
}

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

SrtSource::SrtSource(QObject *parent)
    : StreamSource(parent)
{
}

SrtSource::~SrtSource()
{
    stop();
}

void SrtSource::setConfig(const StreamConfig &config)
{
    m_config = config.srt;
}

VideoCodec SrtSource::negotiatedVideoCodec() const
{
    return m_videoCodec.load(std::memory_order_relaxed);
}

AudioCodec SrtSource::negotiatedAudioCodec() const
{
    return m_audioCodec.load(std::memory_order_relaxed);
}

QByteArray SrtSource::negotiatedAudioExtradata() const
{
    return m_audioExtradata;
}

int SrtSource::negotiatedAudioSampleRate() const
{
    return m_audioSampleRate.load(std::memory_order_relaxed);
}

int SrtSource::negotiatedAudioChannels() const
{
    return m_audioChannels.load(std::memory_order_relaxed);
}

QByteArray SrtSource::buildAacSpecificConfig(int sampleRate, int channels)
{
    // The MPEG-TS demuxer hands ADTS AAC to us with no extradata, but FFmpeg's RTP muxer
    // refuses AAC without global headers. An AudioSpecificConfig (ISO 14496-3) is only five
    // bytes for the common case: 5-bit audio object type, 4-bit sampling-frequency index,
    // 4-bit channel configuration, then the bitstream is zero-padded to a byte boundary.
    // The object type is fixed to AAC-LC (2); every SRT encoder in practice emits LC, and
    // the demuxer does not report the profile for ADTS anyway.
    static const int kRates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                  16000, 12000, 11025, 8000, 7350 };
    int freqIndex = -1;
    for (int i = 0; i < int(std::size(kRates)); ++i) {
        if (kRates[i] == sampleRate) {
            freqIndex = i;
            break;
        }
    }
    if (freqIndex < 0 || channels < 1 || channels > 8) {
        return {};
    }

    // MSB-first layout: [15..11]=object type (2), [10..7]=freq index, [6..3]=channel
    // config, [2..0]=zero pad bits. AAC-LC/48 kHz/stereo yields 0x11 0x90, matching FFmpeg.
    const int objectType = 2;
    const int bits = (objectType << 11) | (freqIndex << 7) | (channels << 3);
    const std::uint8_t bytes[2] = {
        std::uint8_t((bits >> 8) & 0xFF),
        std::uint8_t(bits & 0xFF),
    };
    return QByteArray(reinterpret_cast<const char *>(bytes), 2);
}

int SrtSource::interruptCallback(void *opaque)
{
    return static_cast<SrtSource *>(opaque)->m_stop.load(std::memory_order_relaxed) ? 1 : 0;
}

void SrtSource::start()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_stop.store(false, std::memory_order_relaxed);
    m_thread = std::thread([this] { run(); });
}

void SrtSource::stop()
{
    m_stop.store(true, std::memory_order_relaxed);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool SrtSource::run()
{
    m_audioCodec.store(AudioCodec::Unknown, std::memory_order_relaxed);
    m_audioSampleRate.store(48000, std::memory_order_relaxed);
    m_audioChannels.store(2, std::memory_order_relaxed);
    m_audioExtradata.clear();

    const bool listener = m_config.mode == SrtSourceConfig::Mode::Listener;
    // A listener with no explicit host binds every IPv4 interface. The host part must not be
    // empty: it would resolve to the IPv6 wildcard, which SRT refuses to bind (SRT_EINVOP)
    // because FFmpeg does not set SRTO_IPV6ONLY on the socket.
    QString host = m_config.host.trimmed();
    if (listener && host.isEmpty()) {
        host = u"0.0.0.0"_s;
    }
    const QString url = u"srt://%1:%2"_s.arg(host).arg(m_config.port);

    AVDictionary *options = nullptr;
    av_dict_set(&options, "mode", listener ? "listener" : "caller", 0);
    if (!m_config.passphrase.isEmpty()) {
        const QByteArray passphrase = m_config.passphrase.toUtf8();
        av_dict_set(&options, "passphrase", passphrase.constData(), 0);
    }
    if (m_config.latencyMs > 0) {
        av_dict_set_int(&options, "latency", int64_t(m_config.latencyMs) * 1000, 0);
    }
    // Only a caller passes its streamid to the listener it dials out to (MediaMTX routes
    // reads on its SRT port by "read:<path>"[:user:pass]). A listener's own value would not
    // reach the peer that connects in, so leave it unset there.
    if (!listener && !m_config.streamId.isEmpty()) {
        const QByteArray streamId = m_config.streamId.toUtf8();
        av_dict_set(&options, "streamid", streamId.constData(), 0);
    }

    setState(StreamState::Connecting);

    AVFormatContext *format = avformat_alloc_context();
    if (!format) {
        Q_EMIT errorOccurred(u"Failed to allocate the FFmpeg format context."_s);
        setState(StreamState::Failed);
        return false;
    }
    // Consulted on every ~100 ms polling iteration of both the open (accept/connect) and
    // read paths, so a stop request is noticed quickly no matter where we are blocked.
    format->interrupt_callback.callback = &SrtSource::interruptCallback;
    format->interrupt_callback.opaque = this;

    int ret = avformat_open_input(&format, url.toUtf8().constData(), nullptr, &options);
    if (ret < 0) {
        if (!m_stop.load(std::memory_order_relaxed)) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] {};
            av_strerror(ret, errbuf, sizeof(errbuf));
            QString message = u"Failed to open %1: %2"_s.arg(url, QString::fromUtf8(errbuf));
            if (!listener) {
                // A refused SRT connection surfaces as a generic "I/O error"; the usual causes
                // are server-side (SRT disabled, wrong port, firewall), so say so explicitly.
                message += u" Check that the server has SRT enabled and is reachable on that port."_s;
            }
            Q_EMIT errorOccurred(message);
            setState(StreamState::Failed);
        }
        avformat_free_context(format);
        return false;
    }

    // Identify the streams. The passthrough contract is H.264/H.265 video plus Opus or AAC
    // audio, so anything else is rejected here rather than producing broken output downstream.
    int videoIndex = -1;
    int audioIndex = -1;
    VideoCodec codec = VideoCodec::Unknown;
    AudioCodec audioCodec = AudioCodec::Unknown;
    QByteArray audioExtradata;

    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream *stream = format->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            switch (stream->codecpar->codec_id) {
            case AV_CODEC_ID_H264:
                if (videoIndex < 0) {
                    videoIndex = int(i);
                    codec = VideoCodec::H264;
                }
                break;
            case AV_CODEC_ID_HEVC:
                if (videoIndex < 0) {
                    videoIndex = int(i);
                    codec = VideoCodec::H265;
                }
                break;
            default:
                Q_EMIT errorOccurred(
                    u"SRT stream carries %1 video, only H.264 and H.265 are supported."_s
                        .arg(QString::fromUtf8(avcodec_get_name(stream->codecpar->codec_id))));
                setState(StreamState::Failed);
                avformat_close_input(&format);
                return false;
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            AudioCodec thisAudioCodec;
            switch (stream->codecpar->codec_id) {
            case AV_CODEC_ID_OPUS:
                thisAudioCodec = AudioCodec::Opus;
                break;
            case AV_CODEC_ID_AAC:
                thisAudioCodec = AudioCodec::Aac;
                break;
            case AV_CODEC_ID_AAC_LATM:
                thisAudioCodec = AudioCodec::AacLatm;
                break;
            default:
                Q_EMIT errorOccurred(
                    u"SRT stream carries %1 audio, only Opus and AAC are supported."_s
                        .arg(QString::fromUtf8(avcodec_get_name(stream->codecpar->codec_id))));
                setState(StreamState::Failed);
                avformat_close_input(&format);
                return false;
            }
            if (audioIndex < 0) {
                audioIndex = int(i);
                audioCodec = thisAudioCodec;
                // The demuxer may or may not surface extradata (ADTS carries none, LATM
                // sometimes does). Pass through whatever it reports; the sinks synthesise
                // what they still need (see buildAacSpecificConfig).
                if (stream->codecpar->extradata && stream->codecpar->extradata_size > 0) {
                    audioExtradata = QByteArray(
                        reinterpret_cast<const char *>(stream->codecpar->extradata),
                        stream->codecpar->extradata_size);
                }
            }
        }
    }

    if (videoIndex < 0) {
        Q_EMIT errorOccurred(u"SRT stream has no H.264/H.265 video stream."_s);
        setState(StreamState::Failed);
        avformat_close_input(&format);
        return false;
    }

    m_videoCodec.store(codec, std::memory_order_relaxed);
    Q_EMIT videoCodecNegotiated(codec);

    // Publish the audio codec before Running so the pipeline reads it when it opens the
    // sinks on the first keyframe. ADTS AAC arrives with no extradata, so synthesise the
    // AudioSpecificConfig the RTP muxer needs from the reported rate and channel count.
    if (audioIndex >= 0) {
        if (audioCodec == AudioCodec::Aac && audioExtradata.isEmpty()) {
            audioExtradata = buildAacSpecificConfig(
                format->streams[audioIndex]->codecpar->sample_rate,
                format->streams[audioIndex]->codecpar->ch_layout.nb_channels);
        }
        m_audioExtradata = audioExtradata;
        m_audioCodec.store(audioCodec, std::memory_order_relaxed);
        // Opus's RTP clock is fixed at 48 kHz regardless of content; AAC runs at its own
        // rate, which the demuxer reports (fall back to 48 kHz if it does not).
        const int reportedRate = format->streams[audioIndex]->codecpar->sample_rate;
        const int reportedChannels = format->streams[audioIndex]->codecpar->ch_layout.nb_channels;
        m_audioSampleRate.store(audioCodec == AudioCodec::Opus ? 48000
                                      : (reportedRate > 0 ? reportedRate : 48000),
                                std::memory_order_relaxed);
        m_audioChannels.store(reportedChannels > 0 ? reportedChannels : 2,
                              std::memory_order_relaxed);
    }

    setState(StreamState::Running);

    // The sinks expect RTP clocks: 90 kHz for H.264/H.265 and the audio's own sample rate
    // for the audio stream (48 kHz for Opus, which is fixed; AAC may be 44.1 kHz etc.).
    // Packets from the TS demuxer already carry those time bases, but rescale anyway so
    // other containers (e.g. raw H.264) land on the right clock too.
    const AVRational videoClock = AVRational { 1, 90000 };
    int audioRate = format->streams[audioIndex >= 0 ? audioIndex : videoIndex]->codecpar->sample_rate;
    if (audioRate <= 0) {
        audioRate = 48000;
    }
    const AVRational audioClock = AVRational { 1, audioRate };

    AVPacket *packet = av_packet_alloc();
    quint32 lastVideoTs = 0;
    bool haveLastVideoTs = false;
    quint32 lastAudioTs = 0;
    bool haveLastAudioTs = false;
    // Per-frame increment for the no-timestamp fallback: Opus runs 20 ms frames on the
    // fixed 48 kHz clock (960), AAC frames are 1024 samples on the stream's own clock.
    const quint32 audioFrameStep = audioCodec == AudioCodec::Opus ? 960u : 1024u;

    for (;;) {
        if (m_stop.load(std::memory_order_relaxed)) {
            break;
        }

        ret = av_read_frame(format, packet);
        if (ret < 0) {
            // A peer disconnect surfaces as a read error here. The pipeline's reconnect timer
            // restarts the source with backoff unless we are stopping on purpose.
            if (!m_stop.load(std::memory_order_relaxed)) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] {};
                av_strerror(ret, errbuf, sizeof(errbuf));
                Q_EMIT errorOccurred(u"SRT read failed: %1"_s.arg(QString::fromUtf8(errbuf)));
                setState(StreamState::Failed);
            }
            break;
        }

        if (packet->stream_index == videoIndex && packet->size > 0) {
            const int64_t dts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
            quint32 ts;
            if (dts >= 0) {
                ts = quint32(av_rescale_q(dts, format->streams[videoIndex]->time_base, videoClock));
            } else {
                // No presentation info at all: keep the RTP clock advancing at 30 fps.
                ts = haveLastVideoTs ? lastVideoTs + 3000 : 0;
            }
            lastVideoTs = ts;
            haveLastVideoTs = true;
            m_onVideo(packet->data, std::size_t(packet->size), ts);
        } else if (audioIndex >= 0 && packet->stream_index == audioIndex && packet->size > 0) {
            // The pipeline only registers an audio callback when the stream has audio enabled.
            if (m_onAudio) {
                const int64_t dts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
                quint32 ts;
                if (dts >= 0) {
                    ts = quint32(av_rescale_q(dts, format->streams[audioIndex]->time_base, audioClock));
                } else {
                    // No presentation info: advance by one frame on the audio clock.
                    ts = haveLastAudioTs ? lastAudioTs + audioFrameStep : 0;
                }
                lastAudioTs = ts;
                haveLastAudioTs = true;
                m_onAudio(packet->data, std::size_t(packet->size), ts);
            }
        }

        av_packet_unref(packet);
    }

    avformat_close_input(&format);
    av_packet_free(&packet);
    return false;
}

} // namespace CBridge
