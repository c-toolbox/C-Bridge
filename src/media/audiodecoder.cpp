/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/audiodecoder.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <cstring>
#include <vector>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

/// Builds a standard OpusHead (RFC 7845) with mapping family 0. FFmpeg's internal decoder, when
/// handed no extradata, looks for an in-band header and misreads the first raw packet's TOC byte
/// as a multichannel configuration block; supplying this header makes it accept plain RFC 6716.
std::vector<std::uint8_t> makeOpusHead(int channels, int sampleRate)
{
    std::vector<std::uint8_t> head(21, 0);
    std::memcpy(head.data(), "OpusHead", 8); // magic string
    head[8] = 1;                             // version
    head[9] = static_cast<std::uint8_t>(channels); // channel count (mapping family 0: 1 or 2)
    const std::uint32_t rate = sampleRate > 0 ? static_cast<std::uint32_t>(sampleRate) : 48000;
    head[14] = static_cast<std::uint8_t>(rate & 0xFF); // input sample rate, little-endian
    head[15] = static_cast<std::uint8_t>((rate >> 8) & 0xFF);
    head[16] = static_cast<std::uint8_t>((rate >> 16) & 0xFF);
    head[17] = static_cast<std::uint8_t>((rate >> 24) & 0xFF);
    // pre-skip [10..13] and output gain [18..19] stay zero
    head[20] = 0; // mapping family: 0 selects the standard channel mapping
    return head;
}

/// A little slack past the header so any over-reading parser stays inside allocated memory.
constexpr std::size_t kExtradataPadding = 16;

} // namespace

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder()
{
    close();
}

int AudioDecoder::sampleRate() const
{
    return m_context ? m_context->sample_rate : 0;
}

int AudioDecoder::channels() const
{
    return m_context ? m_context->ch_layout.nb_channels : 0;
}

bool AudioDecoder::open(int sampleRate, int channels, QString *error)
{
    return open(AudioCodec::Opus, sampleRate, channels, QByteArray(), error);
}

bool AudioDecoder::open(AudioCodec codec, int sampleRate, int channels,
                        const QByteArray &extradata, QString *error)
{
    close();
    m_isOpus = codec == AudioCodec::Opus || codec == AudioCodec::Unknown;

    const AVCodec *decoder = nullptr;
    bool usingInternalOpusDecoder = false;
    if (m_isOpus) {
        // Prefer the libopus wrapper: it decodes raw RFC 6716 packets directly and never
        // mistakes a leading TOC byte for an in-band configuration header. When this FFmpeg
        // build has no such decoder, fall back to the internal one, which needs a valid
        // OpusHead (see below) before it will accept unencapsulated network streams.
        decoder = avcodec_find_decoder_by_name("libopus");
        usingInternalOpusDecoder = true;
        if (decoder && decoder->id == AV_CODEC_ID_OPUS) {
            usingInternalOpusDecoder = false;
        } else {
            decoder = avcodec_find_decoder(AV_CODEC_ID_OPUS);
        }
    } else {
        // FFmpeg's native aac decoder handles ADTS, raw ASC-framed and LATM streams
        // uniformly and tolerates the truncated frames that are routine on lossy SRT
        // links, so it is preferred over any hwaccel/libfdk wrapper.
        decoder = avcodec_find_decoder(codec == AudioCodec::AacLatm ? AV_CODEC_ID_AAC_LATM
                                                                     : AV_CODEC_ID_AAC);
    }

    if (!decoder) {
        if (error) {
            *error = m_isOpus ? u"No Opus decoder is available in this FFmpeg build"_s
                              : u"No AAC decoder is available in this FFmpeg build"_s;
        }
        return false;
    }
    m_decoderName = QString::fromUtf8(decoder->name);

    m_context.reset(avcodec_alloc_context3(decoder));
    if (!m_context) {
        if (error) {
            *error = u"Out of memory allocating the audio decoder"_s;
        }
        return false;
    }

    m_context->sample_rate = sampleRate;
    av_channel_layout_default(&m_context->ch_layout, channels);
    m_context->request_sample_fmt = AV_SAMPLE_FMT_FLTP;

    if (usingInternalOpusDecoder) {
        // Without extradata the internal decoder hunts for an in-band OpusHead and misreads the
        // first raw packet's TOC byte as a multichannel configuration header, then aborts. A
        // standard header with mapping family 0 tells it to treat the stream as plain RFC 6716.
        const std::vector<std::uint8_t> head = makeOpusHead(channels, sampleRate);
        m_context->extradata = static_cast<std::uint8_t *>(
            av_mallocz(head.size() + kExtradataPadding));
        if (!m_context->extradata) {
            if (error) {
                *error = u"Out of memory allocating the Opus header"_s;
            }
            m_context.reset();
            return false;
        }
        std::memcpy(m_context->extradata, head.data(), head.size());
        m_context->extradata_size = int(head.size());
    } else if (!m_isOpus && !extradata.isEmpty()) {
        // AAC in a TS container usually arrives as ADTS, which is self-describing and needs
        // no extradata. LATM/LOAS (or a demuxer that already extracted the config) hands us
        // the AudioSpecificConfig, which the decoder needs before the first frame.
        m_context->extradata = static_cast<std::uint8_t *>(
            av_mallocz(size_t(extradata.size()) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!m_context->extradata) {
            if (error) {
                *error = u"Out of memory allocating the AAC config"_s;
            }
            m_context.reset();
            return false;
        }
        std::memcpy(m_context->extradata, extradata.constData(), size_t(extradata.size()));
        m_context->extradata_size = int(extradata.size());
    }

    const int ret = avcodec_open2(m_context.get(), decoder, nullptr);
    if (ret < 0) {
        if (error) {
            *error = u"Could not open the %1 decoder: %2"_s
                         .arg(m_isOpus ? u"Opus"_s : u"AAC"_s, avErrorString(ret));
        }
        m_context.reset();
        return false;
    }

    m_frame = makeFrame();
    m_packet = makePacket();
    m_fltpFrame = makeFrame();
    if (!m_frame || !m_packet || !m_fltpFrame) {
        if (error) {
            *error = u"Out of memory allocating audio buffers"_s;
        }
        close();
        return false;
    }

    return true;
}

void AudioDecoder::close()
{
    m_resampler.reset();
    m_fltpFrame.reset();
    m_frame.reset();
    m_packet.reset();
    m_context.reset();
    m_decoderName.clear();
}

bool AudioDecoder::decode(const std::uint8_t *data, std::size_t size, std::int64_t pts,
                          QString *error)
{
    if (!m_context || !data || size == 0) {
        return false;
    }

    // Some upstream encoders (observed on the WHEP/RTSP feeds we consume) append one undeclared
    // extra byte to their Opus RTP payloads, which makes two-CBR-frame packets structurally
    // invalid: opus_packet_parse_impl() rejects any odd length after the TOC byte. Try the full
    // packet first and fall back to dropping the trailing byte before giving up on a frame.
    // This is an Opus-only quirk; AAC frames are length-delimited and must not be truncated.
    if (decodePacket(data, size, pts, error)) {
        return true;
    }
    if (m_isOpus && size > 1) {
        QString retryError;
        if (decodePacket(data, size - 1, pts, &retryError)) {
            ++m_trailingByteRetries;
            return true;
        }
    }
    // *error was already set by the first attempt.
    return false;
}

bool AudioDecoder::decodePacket(const std::uint8_t *data, std::size_t size, std::int64_t pts,
                                QString *error)
{
    if (!m_context || size == 0) {
        return false;
    }

    av_packet_unref(m_packet.get());
    if (av_new_packet(m_packet.get(), int(size)) < 0) {
        if (error) {
            *error = u"Out of memory allocating an audio packet"_s;
        }
        return false;
    }
    std::memcpy(m_packet->data, data, size);
    m_packet->pts = pts;

    int sendResult = avcodec_send_packet(m_context.get(), m_packet.get());
    if (sendResult == AVERROR(EAGAIN)) {
        // The input buffer is still busy with a previous packet: drain its output first, then
        // retry the send once. Dropping the packet here would lose audio silently.
        bool drainedCleanly = true;
        while (true) {
            const int delivered = receiveAndDeliverFrame(error);
            if (delivered <= 0) {
                drainedCleanly = delivered == 0;
                break;
            }
        }
        if (!drainedCleanly) {
            return false; // *error already set by receiveAndDeliverFrame()
        }
        sendResult = avcodec_send_packet(m_context.get(), m_packet.get());
    }
    av_packet_unref(m_packet.get());

    if (sendResult < 0) {
        if (error) {
            *error = u"Audio decoder rejected a packet: %1"_s.arg(avErrorString(sendResult));
        }
        return false;
    }

    while (true) {
        const int delivered = receiveAndDeliverFrame(error);
        if (delivered <= 0) {
            return delivered == 0; // no more pending frames: success, or error already reported
        }
    }
}

int AudioDecoder::receiveAndDeliverFrame(QString *error)
{
    const int ret = avcodec_receive_frame(m_context.get(), m_frame.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 0; // no more frames pending right now
    }
    if (ret < 0) {
        if (error) {
            *error = u"Audio decode failed: %1"_s.arg(avErrorString(ret));
        }
        return -1;
    }

    AVFrame *out = m_frame.get();
    bool converted = false;
    if (m_frame->format != AV_SAMPLE_FMT_FLTP) {
        out = toPlanarFloat(m_frame.get(), error);
        converted = true;
        if (!out) {
            av_frame_unref(m_frame.get());
            return -1; // *error already set by toPlanarFloat()
        }
    }

    if (m_onFrame) {
        m_onFrame(out);
    }
    av_frame_unref(m_frame.get());
    if (converted) {
        av_frame_unref(m_fltpFrame.get());
    }
    return 1;
}

AVFrame *AudioDecoder::toPlanarFloat(AVFrame *frame, QString *error)
{
    const int inFormat = frame->format;
    const int inChannels = frame->ch_layout.nb_channels;
    const int inRate = frame->sample_rate;

    // (Re)configure the resampler only when the decoded parameters change. For Opus these are
    // fixed after the first frame, so this runs once and is then reused for every packet.
    if (!m_resampler || m_resampleInFormat != inFormat || m_resampleInChannels != inChannels
        || m_resampleInRate != inRate) {
        SwrContext *resampler = nullptr;
        const int ret = swr_alloc_set_opts2(&resampler, &frame->ch_layout, AV_SAMPLE_FMT_FLTP, inRate,
                                            &frame->ch_layout, static_cast<AVSampleFormat>(inFormat),
                                            inRate, 0, nullptr);
        if (ret < 0 || !resampler) {
            if (error) {
                *error = u"Could not configure the audio resampler"_s;
            }
            return nullptr;
        }

        const int initRet = swr_init(resampler);
        if (initRet < 0) {
            swr_free(&resampler);
            if (error) {
                *error = u"Could not initialise the audio resampler: %1"_s.arg(avErrorString(initRet));
            }
            return nullptr;
        }

        m_resampler.reset(resampler);
        m_resampleInFormat = inFormat;
        m_resampleInChannels = inChannels;
        m_resampleInRate = inRate;
    }

    if (!m_fltpFrame) {
        m_fltpFrame = makeFrame();
    }
    av_frame_unref(m_fltpFrame.get());
    m_fltpFrame->format = AV_SAMPLE_FMT_FLTP;
    m_fltpFrame->sample_rate = inRate;
    av_channel_layout_copy(&m_fltpFrame->ch_layout, &frame->ch_layout);
    m_fltpFrame->nb_samples = frame->nb_samples;

    if (av_frame_get_buffer(m_fltpFrame.get(), 0) < 0) {
        if (error) {
            *error = u"Out of memory allocating the converted audio buffer"_s;
        }
        return nullptr;
    }

    const int outSamples = swr_convert(m_resampler.get(), m_fltpFrame->data, m_fltpFrame->nb_samples,
                                       frame->data, frame->nb_samples);
    if (outSamples <= 0) {
        if (error) {
            *error = u"Audio resampling failed"_s;
        }
        return nullptr;
    }

    m_fltpFrame->nb_samples = outSamples;
    return m_fltpFrame.get();
}

} // namespace CBridge
