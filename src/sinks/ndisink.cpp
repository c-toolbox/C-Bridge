/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sinks/ndisink.h"

#include "ndi/ndiruntime.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

#ifdef CBRIDGE_NDI_SUPPORT
#include <Processing.NDI.Lib.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

constexpr int kRtpVideoClock = 90000;
constexpr int kRtpAudioClock = 48000;
constexpr std::int64_t kRtpWrap = 1LL << 32;

} // namespace

NdiSink::NdiSink(NdiSinkConfig config, QString streamName)
    : m_config(std::move(config))
    , m_streamName(std::move(streamName))
{
    if (m_config.senderName.trimmed().isEmpty()) {
        m_config.senderName = m_streamName;
    }
}

NdiSink::~NdiSink()
{
    close();
}

QString NdiSink::describe() const
{
    return u"NDI %1"_s.arg(m_config.senderName);
}

std::int64_t NdiSink::toNdiTimecode(std::int64_t ticks, int clockRate) const
{
    // NDI timecodes are in 100 ns units.
    return av_rescale(ticks, 10'000'000, clockRate);
}

bool NdiSink::open(const StreamFormat &format, QString *error)
{
    close();

#ifndef CBRIDGE_NDI_SUPPORT
    if (error) {
        *error = u"This build was compiled without NDI support"_s;
    }
    return false;
#else
    if (!NdiRuntime::instance().ensureLoaded()) {
        if (error) {
            *error = NdiRuntime::instance().lastError();
        }
        return false;
    }

    if (!m_videoDecoder.open(format.videoCodec, error)) {
        return false;
    }
    m_videoDecoder.setFrameCallback([this](AVFrame *frame) { onDecodedVideo(frame); });
    m_videoFilter.setFrameCallback([this](AVFrame *frame) { onFilteredVideo(frame); });

    if (format.hasAudio && m_config.audioEnabled) {
        QString audioError;
        if (!m_audioDecoder.open(format.audioSampleRate, format.audioChannels, &audioError)) {
            qWarning("NDI sink %s: audio disabled: %s", qUtf8Printable(describe()),
                     qUtf8Printable(audioError));
        } else {
            m_audioDecoder.setFrameCallback([this](AVFrame *frame) { onDecodedAudio(frame); });
        }
    }

    m_videoLastRaw = -1;
    m_videoOffset = 0;
    m_ptsBase = -1;
    m_filterReady = false;
    m_filterFailed = false;
    m_audioFailureLogged = false;
    m_open = true;

    // The sender is created lazily: the real frame size is only known once the
    // first frame has been decoded and filtered.
    return true;
#endif
}

bool NdiSink::ensureSender(int width, int height, QString *error)
{
#ifndef CBRIDGE_NDI_SUPPORT
    Q_UNUSED(width)
    Q_UNUSED(height)
    Q_UNUSED(error)
    return false;
#else
    if (m_sender && width == m_senderWidth && height == m_senderHeight) {
        return true;
    }

    const NDIlib_v5 *lib = NdiRuntime::instance().lib();
    if (!lib) {
        return false;
    }

    if (m_sender) {
        lib->send_destroy(m_sender);
        m_sender = nullptr;
    }

    const QByteArray name = m_config.senderName.toUtf8();
    const QByteArray groups = m_config.groups.toUtf8();

    NDIlib_send_create_t settings {};
    settings.p_ndi_name = name.constData();
    settings.p_groups = groups.isEmpty() ? nullptr : groups.constData();
    // The WebRTC source already paces the stream, so NDI must not re-clock it.
    settings.clock_video = false;
    settings.clock_audio = false;

    m_sender = lib->send_create(&settings);
    if (!m_sender) {
        if (error) {
            *error = u"Could not create the NDI sender \"%1\""_s.arg(m_config.senderName);
        }
        return false;
    }

    m_senderWidth = width;
    m_senderHeight = height;

    const std::size_t bytes = std::size_t(width) * std::size_t(height) * 2; // UYVY
    m_frameBuffers[0].assign(bytes, 0);
    m_frameBuffers[1].assign(bytes, 0);
    m_activeBuffer = 0;

    qInfo("NDI sender \"%s\" ready at %dx%d", name.constData(), width, height);
    return true;
#endif
}

void NdiSink::close()
{
#ifdef CBRIDGE_NDI_SUPPORT
    if (m_sender) {
        const NDIlib_v5 *lib = NdiRuntime::instance().lib();
        if (lib) {
            // Flush the in-flight async frame before freeing its buffer.
            lib->send_send_video_async_v2(m_sender, nullptr);
            lib->send_destroy(m_sender);
        }
        m_sender = nullptr;
    }
#endif

    m_videoFilter.close();
    m_videoDecoder.close();
    m_audioDecoder.close();

    m_frameBuffers[0].clear();
    m_frameBuffers[1].clear();
    m_audioBuffer.clear();

    m_senderWidth = 0;
    m_senderHeight = 0;
    m_filterReady = false;
    m_filterFailed = false;
    m_open = false;
}

bool NdiSink::writeVideo(const std::uint8_t *data, std::size_t size,
                         std::uint32_t rtpTimestamp, bool)
{
    if (!m_videoDecoder.isOpen() || size == 0) {
        return false;
    }

    const std::int64_t raw = std::int64_t(rtpTimestamp);
    if (m_videoLastRaw >= 0) {
        const std::int64_t delta = raw - m_videoLastRaw;
        if (delta < -(kRtpWrap / 2)) {
            m_videoOffset += kRtpWrap;
        } else if (delta > (kRtpWrap / 2)) {
            m_videoOffset -= kRtpWrap;
        }
    }
    m_videoLastRaw = raw;

    const std::int64_t absolute = raw + m_videoOffset;
    if (m_ptsBase < 0) {
        m_ptsBase = absolute;
    }

    const std::int64_t pts = absolute - m_ptsBase;
    m_currentTimecode = toNdiTimecode(pts, kRtpVideoClock);

    QString error;
    if (!m_videoDecoder.decode(data, size, pts, &error)) {
        qWarning("NDI sink %s: %s", qUtf8Printable(describe()), qUtf8Printable(error));
        return false;
    }
    return true;
}

void NdiSink::onDecodedVideo(AVFrame *frame)
{
    if (!m_filterReady) {
        if (m_filterFailed) {
            return;
        }
        QString error;
        if (!m_videoFilter.open(frame, AVRational { 1, kRtpVideoClock },
                                m_config.targetWidth, m_config.targetHeight,
                                m_config.fpsNum, m_config.fpsDen,
                                AV_PIX_FMT_UYVY422, &error)) {
            m_filterFailed = true;
            qWarning("NDI sink %s: %s", qUtf8Printable(describe()), qUtf8Printable(error));
            return;
        }
        m_filterReady = true;
    }

    QString error;
    if (!m_videoFilter.push(frame, &error)) {
        qWarning("NDI sink %s: %s", qUtf8Printable(describe()), qUtf8Printable(error));
    }
}

void NdiSink::onFilteredVideo(AVFrame *frame)
{
#ifdef CBRIDGE_NDI_SUPPORT
    QString error;
    if (!ensureSender(frame->width, frame->height, &error)) {
        if (!error.isEmpty()) {
            qWarning("NDI sink %s: %s", qUtf8Printable(describe()), qUtf8Printable(error));
        }
        return;
    }

    const NDIlib_v5 *lib = NdiRuntime::instance().lib();
    if (!lib) {
        return;
    }

    const int stride = frame->width * 2;
    std::vector<std::uint8_t> &buffer = m_frameBuffers[m_activeBuffer];
    if (buffer.size() < std::size_t(stride) * std::size_t(frame->height)) {
        buffer.assign(std::size_t(stride) * std::size_t(frame->height), 0);
    }

    // Copy row by row: the filter output is padded to FFmpeg's alignment.
    for (int row = 0; row < frame->height; ++row) {
        std::memcpy(buffer.data() + std::size_t(row) * std::size_t(stride),
                    frame->data[0] + std::size_t(row) * std::size_t(frame->linesize[0]),
                    std::size_t(stride));
    }

    NDIlib_video_frame_v2_t videoFrame {};
    videoFrame.xres = frame->width;
    videoFrame.yres = frame->height;
    videoFrame.FourCC = NDIlib_FourCC_video_type_UYVY;
    videoFrame.frame_rate_N = m_config.fpsNum > 0 ? m_config.fpsNum : 60000;
    videoFrame.frame_rate_D = m_config.fpsNum > 0 ? std::max(1, m_config.fpsDen) : 1001;
    videoFrame.picture_aspect_ratio = float(frame->width) / float(frame->height);
    videoFrame.frame_format_type = NDIlib_frame_format_type_progressive;
    videoFrame.timecode = m_currentTimecode;
    videoFrame.p_data = buffer.data();
    videoFrame.line_stride_in_bytes = stride;

    lib->send_send_video_async_v2(m_sender, &videoFrame);
    m_activeBuffer = 1 - m_activeBuffer;

    m_bytesWritten.fetch_add(std::size_t(stride) * std::size_t(frame->height),
                             std::memory_order_relaxed);
#else
    Q_UNUSED(frame)
#endif
}

bool NdiSink::writeAudio(const std::uint8_t *data, std::size_t size,
                         std::uint32_t rtpTimestamp)
{
    if (!m_audioDecoder.isOpen() || size == 0 || m_audioFailureLogged) {
        return false;
    }

    QString error;
    if (!m_audioDecoder.decode(data, size, std::int64_t(rtpTimestamp), &error)) {
        m_audioFailureLogged = true;
        qWarning("NDI sink %s: audio disabled after a decode failure: %s",
                 qUtf8Printable(describe()), qUtf8Printable(error));
        return false;
    }
    return true;
}

void NdiSink::onDecodedAudio(AVFrame *frame)
{
#ifdef CBRIDGE_NDI_SUPPORT
    if (!m_sender) {
        return;
    }
    const NDIlib_v5 *lib = NdiRuntime::instance().lib();
    if (!lib || frame->format != AV_SAMPLE_FMT_FLTP) {
        return;
    }

    const int channels = frame->ch_layout.nb_channels;
    const int samples = frame->nb_samples;
    if (channels <= 0 || samples <= 0) {
        return;
    }

    // NDI wants one contiguous planar block; FFmpeg gives separate plane pointers.
    m_audioBuffer.resize(std::size_t(channels) * std::size_t(samples));
    for (int channel = 0; channel < channels; ++channel) {
        std::memcpy(m_audioBuffer.data() + std::size_t(channel) * std::size_t(samples),
                    frame->data[channel], std::size_t(samples) * sizeof(float));
    }

    NDIlib_audio_frame_v2_t audioFrame {};
    audioFrame.sample_rate = frame->sample_rate > 0 ? frame->sample_rate : kRtpAudioClock;
    audioFrame.no_channels = channels;
    audioFrame.no_samples = samples;
    audioFrame.timecode = m_currentTimecode;
    audioFrame.p_data = m_audioBuffer.data();
    audioFrame.channel_stride_in_bytes = int(std::size_t(samples) * sizeof(float));

    lib->send_send_audio_v2(m_sender, &audioFrame);
#else
    Q_UNUSED(frame)
#endif
}

} // namespace CBridge
