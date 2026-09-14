#include "sinks/tsmulticastsink.h"

#include <QByteArray>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

constexpr int kRtpVideoClock = 90000; // also the MPEG-TS timebase
constexpr int kRtpAudioClock = 48000; // Opus
constexpr qint64 kRtpWrap = 1LL << 32;

QString avError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

AVCodecID toAvCodecId(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264: return AV_CODEC_ID_H264;
    case VideoCodec::H265: return AV_CODEC_ID_HEVC;
    case VideoCodec::Unknown: break;
    }
    return AV_CODEC_ID_NONE;
}

} // namespace

TsMulticastSink::TsMulticastSink(TsMulticastSinkConfig config)
    : m_config(std::move(config))
{
}

TsMulticastSink::~TsMulticastSink()
{
    close();
}

QString TsMulticastSink::describe() const
{
    return u"TS %1:%2"_s.arg(m_config.groupAddress).arg(m_config.port);
}

bool TsMulticastSink::addVideoStream(const StreamFormat &format, QString *error)
{
    m_videoStream = avformat_new_stream(m_format, nullptr);
    if (!m_videoStream) {
        if (error) {
            *error = u"Could not allocate the TS video stream"_s;
        }
        return false;
    }

    AVCodecParameters *parameters = m_videoStream->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = toAvCodecId(format.videoCodec);
    parameters->width = format.width;
    parameters->height = format.height;

    if (!format.videoExtradata.isEmpty()) {
        parameters->extradata = static_cast<uint8_t *>(
            av_mallocz(size_t(format.videoExtradata.size()) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!parameters->extradata) {
            if (error) {
                *error = u"Out of memory allocating codec extradata"_s;
            }
            return false;
        }
        std::memcpy(parameters->extradata, format.videoExtradata.constData(),
                    size_t(format.videoExtradata.size()));
        parameters->extradata_size = int(format.videoExtradata.size());
    }

    m_videoStream->time_base = AVRational { 1, kRtpVideoClock };
    return true;
}

bool TsMulticastSink::addAudioStream(const StreamFormat &format, QString *error)
{
    m_audioStream = avformat_new_stream(m_format, nullptr);
    if (!m_audioStream) {
        if (error) {
            *error = u"Could not allocate the TS audio stream"_s;
        }
        return false;
    }

    AVCodecParameters *parameters = m_audioStream->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_AUDIO;
    parameters->codec_id = AV_CODEC_ID_OPUS;
    parameters->sample_rate = format.audioSampleRate;
    av_channel_layout_default(&parameters->ch_layout, format.audioChannels);

    m_audioStream->time_base = AVRational { 1, kRtpAudioClock };
    return true;
}

bool TsMulticastSink::open(const StreamFormat &format, QString *error)
{
    close();

    if (format.videoCodec == VideoCodec::Unknown) {
        if (error) {
            *error = u"Cannot open the TS sink without a known video codec"_s;
        }
        return false;
    }

    const QByteArray url = m_config.url().toUtf8();

    int ret = avformat_alloc_output_context2(&m_format, nullptr, "mpegts", url.constData());
    if (ret < 0 || !m_format) {
        if (error) {
            *error = u"Could not create the MPEG-TS muxer: %1"_s.arg(avError(ret));
        }
        return false;
    }

    if (!addVideoStream(format, error)) {
        close();
        return false;
    }
    if (format.hasAudio && !addAudioStream(format, error)) {
        close();
        return false;
    }

    // Short table periods let a receiver that joins mid-stream lock on quickly.
    av_opt_set_int(m_format->priv_data, "pat_period", m_config.patPeriodMs * 1000, 0);
    av_opt_set_int(m_format->priv_data, "sdt_period", m_config.patPeriodMs * 1000, 0);
    av_opt_set_int(m_format->priv_data, "pcr_period", m_config.pcrPeriodMs, 0);
    av_opt_set(m_format->priv_data, "mpegts_flags", "+resend_headers", 0);
    m_format->flags |= AVFMT_FLAG_FLUSH_PACKETS;

    ret = avio_open2(&m_format->pb, url.constData(), AVIO_FLAG_WRITE, nullptr, nullptr);
    if (ret < 0) {
        if (error) {
            *error = u"Could not open %1: %2"_s.arg(m_config.url(), avError(ret));
        }
        close();
        return false;
    }

    ret = avformat_write_header(m_format, nullptr);
    if (ret < 0) {
        if (error) {
            *error = u"Could not write the TS header: %1"_s.arg(avError(ret));
        }
        close();
        return false;
    }

    m_packet = av_packet_alloc();
    if (!m_packet) {
        if (error) {
            *error = u"Out of memory allocating an AVPacket"_s;
        }
        close();
        return false;
    }

    m_videoLastRaw = -1;
    m_audioLastRaw = -1;
    m_videoOffset = 0;
    m_audioOffset = 0;
    m_ptsBase = -1;
    m_open = true;
    return true;
}

void TsMulticastSink::close()
{
    if (m_format && m_open) {
        av_write_trailer(m_format);
    }
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    if (m_format) {
        if (m_format->pb) {
            avio_closep(&m_format->pb);
        }
        avformat_free_context(m_format);
        m_format = nullptr;
    }

    m_videoStream = nullptr;
    m_audioStream = nullptr;
    m_open = false;
}

qint64 TsMulticastSink::unwrap(std::uint32_t rtpTimestamp, qint64 &lastRaw, qint64 &offset) const
{
    const qint64 raw = qint64(rtpTimestamp);

    if (lastRaw >= 0) {
        const qint64 delta = raw - lastRaw;
        if (delta < -(kRtpWrap / 2)) {
            offset += kRtpWrap;
        } else if (delta > (kRtpWrap / 2)) {
            offset -= kRtpWrap;
        }
    }

    lastRaw = raw;
    return raw + offset;
}

bool TsMulticastSink::writePacket(AVStream *stream, const std::uint8_t *data, std::size_t size,
                                  qint64 pts, bool isKeyframe)
{
    av_packet_unref(m_packet);

    if (av_new_packet(m_packet, int(size)) < 0) {
        return false;
    }
    std::memcpy(m_packet->data, data, size);

    m_packet->stream_index = stream->index;
    m_packet->pts = pts;
    m_packet->dts = pts;
    m_packet->duration = 0;
    if (isKeyframe) {
        m_packet->flags |= AV_PKT_FLAG_KEY;
    }

    const int ret = av_interleaved_write_frame(m_format, m_packet);
    av_packet_unref(m_packet);

    if (ret < 0) {
        qWarning("TS sink %s: write failed: %s", qUtf8Printable(describe()),
                 qUtf8Printable(avError(ret)));
        return false;
    }

    m_bytesWritten.fetch_add(size, std::memory_order_relaxed);
    return true;
}

bool TsMulticastSink::writeVideo(const std::uint8_t *data, std::size_t size,
                                 std::uint32_t rtpTimestamp, bool isKeyframe)
{
    if (!m_open || !m_videoStream || size == 0) {
        return false;
    }

    const qint64 absolute = unwrap(rtpTimestamp, m_videoLastRaw, m_videoOffset);
    if (m_ptsBase < 0) {
        m_ptsBase = absolute;
    }

    return writePacket(m_videoStream, data, size, absolute - m_ptsBase, isKeyframe);
}

bool TsMulticastSink::writeAudio(const std::uint8_t *data, std::size_t size,
                                 std::uint32_t rtpTimestamp)
{
    if (!m_open || !m_audioStream || size == 0) {
        return false;
    }
    // Anchoring audio before the first video frame would put it at a negative PTS.
    if (m_ptsBase < 0) {
        return false;
    }

    const qint64 absolute = unwrap(rtpTimestamp, m_audioLastRaw, m_audioOffset);

    // The audio clock is 48 kHz while m_ptsBase is on the 90 kHz video clock, so
    // rebase in the video domain and express the result in the audio timebase.
    const qint64 videoDomain = av_rescale(absolute, kRtpVideoClock, kRtpAudioClock);
    const qint64 rebased = av_rescale(videoDomain - m_ptsBase, kRtpAudioClock, kRtpVideoClock);

    return writePacket(m_audioStream, data, size, rebased, true);
}

} // namespace CBridge
