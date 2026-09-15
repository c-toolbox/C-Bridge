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
}

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

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
    close();

    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!decoder) {
        if (error) {
            *error = u"No Opus decoder is available in this FFmpeg build"_s;
        }
        return false;
    }

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

    const int ret = avcodec_open2(m_context.get(), decoder, nullptr);
    if (ret < 0) {
        if (error) {
            *error = u"Could not open the Opus decoder: %1"_s.arg(avErrorString(ret));
        }
        m_context.reset();
        return false;
    }

    m_frame = makeFrame();
    m_packet = makePacket();
    if (!m_frame || !m_packet) {
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
    m_frame.reset();
    m_packet.reset();
    m_context.reset();
}

bool AudioDecoder::decode(const std::uint8_t *data, std::size_t size, std::int64_t pts,
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

    const int sendResult = avcodec_send_packet(m_context.get(), m_packet.get());
    av_packet_unref(m_packet.get());

    if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
        if (error) {
            *error = u"Audio decoder rejected a packet: %1"_s.arg(avErrorString(sendResult));
        }
        return false;
    }

    while (true) {
        const int ret = avcodec_receive_frame(m_context.get(), m_frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            if (error) {
                *error = u"Audio decode failed: %1"_s.arg(avErrorString(ret));
            }
            return false;
        }

        if (m_onFrame) {
            m_onFrame(m_frame.get());
        }
        av_frame_unref(m_frame.get());
    }
}

} // namespace CBridge
