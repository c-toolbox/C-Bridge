/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sinks/rtpmulticastsink.h"

#include <QByteArray>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

constexpr int kRtpVideoClock = 90000; // H.264/H.265 RTP clock
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

RtpMulticastSink::RtpMulticastSink(RtpMulticastSinkConfig config)
    : m_config(std::move(config))
{
}

RtpMulticastSink::~RtpMulticastSink()
{
    close();
}

QString RtpMulticastSink::describe() const
{
    return u"RTP %1:%2/%3"_s.arg(m_config.groupAddress).arg(int(m_config.port)).arg(int(m_config.port) + 1);
}

QString RtpMulticastSink::buildUrl(const RtpMulticastSinkConfig &config, quint16 port)
{
    // pkt_size caps the RTP payload (the muxer fragments to max_packet_size - 12), ttl
    // scopes the multicast, overrun_nonfatal keeps a congested socket from killing the
    // stream and fifo_size absorbs short bursts. localaddr pins the outgoing interface.
    QString url = u"udp://%1:%2?pkt_size=%3&ttl=%4&overrun_nonfatal=1&fifo_size=5000000"_s
        .arg(config.groupAddress)
        .arg(int(port))
        .arg(config.packetSize)
        .arg(config.ttl);
    if (!config.localAddress.isEmpty()) {
        url += u"&localaddr=%1"_s.arg(config.localAddress);
    }
    return url;
}

bool RtpMulticastSink::openStream(RtpStreamContext &stream, const QString &url, VideoCodec codec,
                                  int width, int height, const std::uint8_t *extradata, std::size_t extradataSize,
                                  int sampleRate, int channels, QString *error)
{
    // Unknown selects the Opus audio stream; H264/H265 select the video stream.
    const AVCodecID codecId = (codec == VideoCodec::Unknown) ? AV_CODEC_ID_OPUS : toAvCodecId(codec);

    const QByteArray utf8Url = url.toUtf8();

    int ret = avformat_alloc_output_context2(&stream.format, nullptr, "rtp", utf8Url.constData());
    if (ret < 0 || !stream.format) {
        if (error) {
            *error = u"Could not create the RTP muxer: %1"_s.arg(avError(ret));
        }
        return false;
    }

    AVStream *avStream = avformat_new_stream(stream.format, nullptr);
    if (!avStream) {
        if (error) {
            *error = u"Could not allocate the RTP stream"_s;
        }
        closeStream(stream);
        return false;
    }

    AVCodecParameters *parameters = avStream->codecpar;
    parameters->codec_id = codecId;
    if (codecId == AV_CODEC_ID_OPUS) {
        parameters->codec_type = AVMEDIA_TYPE_AUDIO;
        parameters->sample_rate = sampleRate;
        av_channel_layout_default(&parameters->ch_layout, channels);
    } else {
        parameters->codec_type = AVMEDIA_TYPE_VIDEO;
        parameters->width = width;
        parameters->height = height;

        if (extradata && extradataSize > 0) {
            // Annex-B parameter sets. The RTP packetizer only treats the extradata as AVCC
            // when it starts with a length-size byte, and Annex-B starts with a start code,
            // so passing it through verbatim is safe.
            parameters->extradata = static_cast<uint8_t *>(
                av_mallocz(extradataSize + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!parameters->extradata) {
                if (error) {
                    *error = u"Out of memory allocating codec extradata"_s;
                }
                closeStream(stream);
                return false;
            }
            std::memcpy(parameters->extradata, extradata, extradataSize);
            parameters->extradata_size = int(extradataSize);
        }
    }

    // The rtp muxer sets its own pts info in write_header (32-bit, 90 kHz video /
    // 48 kHz audio); setting it here documents the clock the timestamps are on.
    avStream->time_base = AVRational { 1, codecId == AV_CODEC_ID_OPUS ? kRtpAudioClock : kRtpVideoClock };

    // Same as the RTSP sink: without this flag FFmpeg 8's RTP muxer holds packets in its
    // interleave queue and nothing reaches the wire until close.
    stream.format->flags |= AVFMT_FLAG_FLUSH_PACKETS;

    ret = avio_open2(&stream.format->pb, utf8Url.constData(), AVIO_FLAG_WRITE, nullptr, nullptr);
    if (ret < 0) {
        if (error) {
            *error = u"Could not open %1: %2"_s.arg(url, avError(ret));
        }
        closeStream(stream);
        return false;
    }

    ret = avformat_write_header(stream.format, nullptr);
    if (ret < 0) {
        if (error) {
            *error = u"Could not write the RTP header: %1"_s.arg(avError(ret));
        }
        closeStream(stream);
        return false;
    }

    stream.packet = av_packet_alloc();
    if (!stream.packet) {
        if (error) {
            *error = u"Out of memory allocating an AVPacket"_s;
        }
        closeStream(stream);
        return false;
    }

    return true;
}

void RtpMulticastSink::closeStream(RtpStreamContext &stream)
{
    if (stream.format && stream.format->pb) {
        av_write_trailer(stream.format); // sends an RTCP BYE when the muxer supports it
        avio_closep(&stream.format->pb);
    }
    if (stream.packet) {
        av_packet_free(&stream.packet);
        stream.packet = nullptr;
    }
    if (stream.format) {
        // Frees the streams, codec parameters and extradata with it.
        avformat_free_context(stream.format);
        stream.format = nullptr;
    }

    stream.lastRaw = -1;
    stream.offset = 0;
    stream.base = -1;
}

bool RtpMulticastSink::open(const StreamFormat &format, QString *error)
{
    close();

    if (format.videoCodec == VideoCodec::Unknown) {
        if (error) {
            *error = u"Cannot open the RTP sink without a known video codec"_s;
        }
        return false;
    }

    const QString videoUrl = buildUrl(m_config, m_config.port);
    if (!openStream(m_video, videoUrl, format.videoCodec, format.width, format.height,
                    reinterpret_cast<const std::uint8_t *>(format.videoExtradata.constData()),
                    size_t(format.videoExtradata.size()), 0, 0, error)) {
        close();
        return false;
    }

    if (format.hasAudio) {
        const QString audioUrl = buildUrl(m_config, quint16(int(m_config.port) + 1));
        if (!openStream(m_audio, audioUrl, VideoCodec::Unknown, 0, 0, nullptr, 0, format.audioSampleRate,
                        format.audioChannels, error)) {
            close();
            return false;
        }
    }

    m_open = true;
    return true;
}

void RtpMulticastSink::close()
{
    closeStream(m_video);
    closeStream(m_audio);
    m_open = false;
}

qint64 RtpMulticastSink::unwrapTimestamp(std::uint32_t rtpTimestamp, RtpStreamContext &stream)
{
    const qint64 raw = qint64(rtpTimestamp);

    if (stream.lastRaw >= 0) {
        const qint64 delta = raw - stream.lastRaw;
        if (delta < -(kRtpWrap / 2)) {
            stream.offset += kRtpWrap;
        } else if (delta > (kRtpWrap / 2)) {
            stream.offset -= kRtpWrap;
        }
    }

    stream.lastRaw = raw;
    return raw + stream.offset;
}

bool RtpMulticastSink::writePacket(RtpStreamContext &stream, const std::uint8_t *data, std::size_t size,
                                   qint64 pts, bool isKeyframe)
{
    av_packet_unref(stream.packet);

    if (av_new_packet(stream.packet, int(size)) < 0) {
        return false;
    }
    std::memcpy(stream.packet->data, data, size);

    stream.packet->stream_index = 0; // each context carries exactly one stream
    stream.packet->pts = pts;
    stream.packet->dts = pts;
    stream.packet->duration = 0;
    if (isKeyframe) {
        stream.packet->flags |= AV_PKT_FLAG_KEY;
    }

    // av_interleaved_write_frame (not av_write_frame): with FFmpeg 8's refactored RTP
    // muxer the plain call only enqueues, and datagrams go out on interleaving.
    const int ret = av_interleaved_write_frame(stream.format, stream.packet);
    av_packet_unref(stream.packet);

    if (ret < 0) {
        qWarning("RTP sink %s: write failed: %s", qUtf8Printable(describe()),
                 qUtf8Printable(avError(ret)));
        return false;
    }

    m_bytesWritten.fetch_add(size, std::memory_order_relaxed);
    return true;
}

bool RtpMulticastSink::writeVideo(const std::uint8_t *data, std::size_t size, std::uint32_t rtpTimestamp,
                                  bool isKeyframe)
{
    if (!m_open || !m_video.format || size == 0) {
        return false;
    }

    const qint64 absolute = unwrapTimestamp(rtpTimestamp, m_video);
    if (m_video.base < 0) {
        m_video.base = absolute;
    }

    // The rtp muxer expects pts in units of the stream clock (90 kHz for video).
    return writePacket(m_video, data, size, absolute - m_video.base, isKeyframe);
}

bool RtpMulticastSink::writeAudio(const std::uint8_t *data, std::size_t size, std::uint32_t rtpTimestamp)
{
    if (!m_open || !m_audio.format || size == 0) {
        return false;
    }

    const qint64 absolute = unwrapTimestamp(rtpTimestamp, m_audio);
    if (m_audio.base < 0) {
        m_audio.base = absolute;
    }

    // Each RTP stream has its own base timestamp and SSRC, so audio is rebased to its
    // first packet independently of the video clock - receivers align streams via RTCP.
    return writePacket(m_audio, data, size, absolute - m_audio.base, true);
}

} // namespace CBridge