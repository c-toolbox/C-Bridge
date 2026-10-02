/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ytdlp/audiotranscoder.h"

extern "C" {
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

constexpr int kOpusRate = 48000;
constexpr int kOpusChannels = 2;

} // namespace

AudioTranscoder::AudioTranscoder() = default;

AudioTranscoder::~AudioTranscoder()
{
    close();
}

bool AudioTranscoder::open(AVCodecID codecId, int sampleRate, int channels, int bitrateKbps,
                           QString *error, const QByteArray &extradata)
{
    close();
    if (sampleRate <= 0 || (channels != 1 && channels != 2)) {
        if (error) {
            *error = u"Unsupported audio input: %1 Hz / %2 channel(s)"_s.arg(sampleRate).arg(channels);
        }
        return false;
    }

    // FFmpeg's native decoders handle the container framing of their format, so no per-
    // container options are needed here.
    const AVCodec *decoder = avcodec_find_decoder(codecId);
    if (!decoder) {
        if (error) {
            *error = u"No %1 decoder in this FFmpeg build"_s.arg(QString::fromUtf8(avcodec_get_name(codecId)));
        }
        return false;
    }
    m_decoder.reset(avcodec_alloc_context3(decoder));
    m_decoder->sample_rate = sampleRate;
    av_channel_layout_default(&m_decoder->ch_layout, channels);
    // Raw AAC (no ADTS header) is decodable only with its AudioSpecificConfig; the pipe path
    // never needs this (TS delivers ADTS) but the direct-URL path may.
    if (!extradata.isEmpty()) {
        m_extradata = extradata;
        m_decoder->extradata =
            static_cast<uint8_t *>(av_malloc(size_t(m_extradata.size()) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!m_decoder->extradata) {
            if (error) {
                *error = u"Could not allocate audio extradata"_s;
            }
            close();
            return false;
        }
        std::memcpy(m_decoder->extradata, m_extradata.constData(), size_t(m_extradata.size()));
        m_decoder->extradata_size = m_extradata.size();
    }
    if (avcodec_open2(m_decoder.get(), decoder, nullptr) < 0) {
        if (error) {
            *error = u"Could not open the AAC decoder"_s;
        }
        close();
        return false;
    }

    // Prefer libopus: it is the encoder every C-Bridge Opus path already assumes. Fall back
    // to any other Opus encoder this build ships with.
    const AVCodec *encoder = avcodec_find_encoder_by_name("libopus");
    if (!encoder || encoder->id != AV_CODEC_ID_OPUS) {
        encoder = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    }
    if (!encoder) {
        if (error) {
            *error = u"No Opus encoder in this FFmpeg build"_s;
        }
        close();
        return false;
    }
    // The libopus wrapper accepts S16 or packed float input but not FLTP; the native opus
    // encoder is the other way around. Pick whichever format this instance supports and do
    // the conversion in encodeBuffered(). sample_fmt must be chosen before avcodec_open2:
    // the wrapper validates it against libopus, which rejects AV_SAMPLE_FMT_NONE (-1).
    auto supportsFormat = [](const AVCodec *enc, AVSampleFormat wanted) -> bool {
        const void *configs = nullptr;
        if (avcodec_get_supported_config(nullptr, enc, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs,
                                         nullptr) < 0 || !configs) {
            return true; // no list: all formats are supported
        }
        const auto *fmts = static_cast<const enum AVSampleFormat *>(configs);
        for (int i = 0; fmts[i] != AV_SAMPLE_FMT_NONE; ++i) {
            if (fmts[i] == wanted) {
                return true;
            }
        }
        return false;
    };
    if (supportsFormat(encoder, AV_SAMPLE_FMT_S16)) {
        m_encFormat = AV_SAMPLE_FMT_S16;
    } else if (supportsFormat(encoder, AV_SAMPLE_FMT_FLTP)) {
        m_encFormat = AV_SAMPLE_FMT_FLTP;
    } else {
        if (error) {
            *error = u"The Opus encoder accepts neither S16 nor FLTP input"_s;
        }
        close();
        return false;
    }

    m_encoder.reset(avcodec_alloc_context3(encoder));
    m_encoder->sample_rate = kOpusRate;
    av_channel_layout_default(&m_encoder->ch_layout, kOpusChannels);
    m_encoder->bit_rate = bitrateKbps * 1000;
    m_encoder->time_base = AVRational { 1, kOpusRate }; // pts in samples
    m_encoder->sample_fmt = static_cast<AVSampleFormat>(m_encFormat);
    m_encoder->frame_size = 960; // one 20 ms Opus frame at 48 kHz
    if (avcodec_open2(m_encoder.get(), encoder, nullptr) < 0) {
        if (error) {
            *error = u"Could not open the Opus encoder"_s;
        }
        close();
        return false;
    }

    m_frame = makeFrame();
    m_fltpFrame = makeFrame();
    m_encFrame = makeFrame();
    m_packet = makePacket();
    m_encPacket = makePacket();
    return true;
}

void AudioTranscoder::close()
{
    m_decoder.reset();
    m_encoder.reset();
    m_resampler.reset();
    m_frame.reset();
    m_fltpFrame.reset();
    m_encFrame.reset();
    m_packet.reset();
    m_encPacket.reset();
    m_extradata.clear();
    m_planes[0].clear();
    m_planes[1].clear();
    m_inFormat = -1;
    m_inChannels = 0;
    m_inRate = 0;
    m_encFormat = -1;
    m_encodedSamples = 0;
}

bool AudioTranscoder::transcode(const std::uint8_t *data, std::size_t size, std::int64_t pts,
                                const PacketCallback &callback, QString *error)
{
    if (!m_encoder || !callback) {
        if (error) {
            *error = u"AudioTranscoder is not open"_s;
        }
        return false;
    }

    // 1. Feed the AAC packet to the decoder. The scratch packet keeps its own reference, so
    //    unref it first: the previous call's data would otherwise leak until reset().
    av_packet_unref(m_packet.get());
    if (av_new_packet(m_packet.get(), int(size)) < 0) {
        if (error) {
            *error = u"Could not allocate the AAC input packet"_s;
        }
        return false;
    }
    std::memcpy(m_packet->data, data, size);
    m_packet->pts = pts;

    const int sendRet = avcodec_send_packet(m_decoder.get(), m_packet.get());
    if (sendRet < 0) {
        if (error) {
            *error = u"AAC decode failed: %1"_s.arg(avErrorString(sendRet));
        }
        return false;
    }

    // 2. Drain every decoded frame into the sample buffer.
    for (;;) {
        av_frame_unref(m_frame.get());
        const int ret = avcodec_receive_frame(m_decoder.get(), m_frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            if (error) {
                *error = u"AAC decode failed: %1"_s.arg(avErrorString(ret));
            }
            return false;
        }
        if (!appendDecodedFrame(m_frame.get(), error)) {
            return false;
        }
    }

    // 3. Encode whatever full Opus frames the buffer now holds.
    return encodeBuffered(callback, error);
}

bool AudioTranscoder::appendDecodedFrame(AVFrame *frame, QString *error)
{
    const int inFormat = frame->format;
    const int inChannels = frame->ch_layout.nb_channels;
    const int inRate = frame->sample_rate;

    // Rebuild the resampler only when the decoded layout actually changes (it does not for a
    // stable YouTube feed, so this is a one-time cost per stream).
    if (!m_resampler || m_inFormat != inFormat || m_inChannels != inChannels || m_inRate != inRate) {
        SwrContext *resampler = nullptr;
        // AV_CHANNEL_LAYOUT_STEREO is a brace-initializer list, not an object: build the real
        // layout with av_channel_layout_default (the codebase's usual pattern).
        AVChannelLayout outLayout {};
        av_channel_layout_default(&outLayout, kOpusChannels);
        const int ret = swr_alloc_set_opts2(&resampler, &outLayout, AV_SAMPLE_FMT_FLTP,
                                            kOpusRate, &frame->ch_layout,
                                            static_cast<AVSampleFormat>(inFormat), inRate, 0, nullptr);
        if (ret < 0 || !resampler) {
            if (error) {
                *error = u"Could not allocate the audio resampler"_s;
            }
            return false;
        }
        if (swr_init(resampler) < 0) {
            swr_free(&resampler);
            if (error) {
                *error = u"Could not initialize the audio resampler"_s;
            }
            return false;
        }
        m_resampler.reset(resampler);
        m_inFormat = inFormat;
        m_inChannels = inChannels;
        m_inRate = inRate;
    }

    av_frame_unref(m_fltpFrame.get());
    m_fltpFrame->format = AV_SAMPLE_FMT_FLTP;
    m_fltpFrame->sample_rate = kOpusRate;
    av_channel_layout_default(&m_fltpFrame->ch_layout, kOpusChannels);
    // Upper bound on the output sample count for this input frame.
    m_fltpFrame->nb_samples = av_rescale_rnd(frame->nb_samples, kOpusRate, inRate, AV_ROUND_UP) + 16;
    if (av_frame_get_buffer(m_fltpFrame.get(), 0) < 0) {
        if (error) {
            *error = u"Could not allocate the resampled audio frame"_s;
        }
        return false;
    }

    const int outSamples = swr_convert(m_resampler.get(), m_fltpFrame->data, m_fltpFrame->nb_samples,
                                       frame->data, frame->nb_samples);
    if (outSamples <= 0) {
        if (error) {
            *error = u"Audio resampling failed"_s;
        }
        return false;
    }

    for (int c = 0; c < kOpusChannels; ++c) {
        const float *plane = reinterpret_cast<const float *>(m_fltpFrame->data[c]);
        m_planes[c].insert(m_planes[c].end(), plane, plane + outSamples);
    }
    return true;
}

bool AudioTranscoder::encodeBuffered(const PacketCallback &callback, QString *error)
{
    const int frameSize = m_encoder->frame_size; // 960 samples: one 20 ms Opus frame at 48 kHz
    if (frameSize <= 0 || m_planes[0].size() < std::size_t(frameSize)) {
        return true; // nothing to encode yet
    }

    for (;;) {
        av_frame_unref(m_encFrame.get());
        m_encFrame->format = static_cast<AVSampleFormat>(m_encFormat);
        m_encFrame->sample_rate = kOpusRate;
        av_channel_layout_default(&m_encFrame->ch_layout, kOpusChannels);
        m_encFrame->nb_samples = frameSize;
        m_encFrame->pts = m_encodedSamples; // encoder time_base is 1/48000: pts in samples
        if (av_frame_get_buffer(m_encFrame.get(), 0) < 0) {
            if (error) {
                *error = u"Could not allocate the Opus input frame"_s;
            }
            return false;
        }
        if (m_encFormat == AV_SAMPLE_FMT_S16) {
            // Packed interleaved S16: convert from the planar float buffer with clamping.
            auto *out = reinterpret_cast<std::int16_t *>(m_encFrame->data[0]);
            for (int n = 0; n < frameSize; ++n) {
                for (int c = 0; c < kOpusChannels; ++c) {
                    const float sample = m_planes[c][std::size_t(n)] * 32768.0f;
                    out[std::size_t(n) * kOpusChannels + c] =
                        static_cast<std::int16_t>(std::clamp(sample, -32768.0f, 32767.0f));
                }
            }
        } else {
            for (int c = 0; c < kOpusChannels; ++c) {
                std::memcpy(m_encFrame->data[c], m_planes[c].data(), std::size_t(frameSize) * sizeof(float));
            }
        }

        const int sendRet = avcodec_send_frame(m_encoder.get(), m_encFrame.get());
        if (sendRet < 0) {
            if (error) {
                *error = u"Opus encode failed: %1"_s.arg(avErrorString(sendRet));
            }
            return false;
        }

        // Drop the consumed samples before receiving, so a failed receive never double-encodes.
        for (int c = 0; c < kOpusChannels; ++c) {
            m_planes[c].erase(m_planes[c].begin(), m_planes[c].begin() + frameSize);
        }
        m_encodedSamples += frameSize;

        for (;;) {
            const int ret = avcodec_receive_packet(m_encoder.get(), m_encPacket.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                if (error) {
                    *error = u"Opus encode failed: %1"_s.arg(avErrorString(ret));
                }
                return false;
            }
            callback(m_encPacket->data, std::size_t(m_encPacket->size));
            av_packet_unref(m_encPacket.get());
        }

        if (m_planes[0].size() < std::size_t(frameSize)) {
            break;
        }
    }
    return true;
}

} // namespace CBridge
