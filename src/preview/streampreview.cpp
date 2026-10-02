/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "preview/streampreview.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QVideoFrame>
#include <QVideoSink>

#include <QImage>

#include <algorithm>
#include <cstring>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

/// Preview frames are capped so a 4K/60 source does not burn the GUI's texture memory or
/// CPU on frames nobody can perceive; the filter's last stage converts to BGR0, which maps
/// 1:1 onto QImage::Format_RGB32.
constexpr int kPreviewFpsNum = 30;
constexpr int kPreviewFpsDen = 1;

/// Longest video frame the preview will render; larger sources are scaled down to fit,
/// keeping the viewer window's texture footprint bounded.
constexpr int kPreviewMaxWidth = 1920;
constexpr int kPreviewMaxHeight = 1080;

/// ~1 s of 48 kHz stereo float at the head of the ring; beyond that the producer sheds the
/// oldest samples so a stalled audio thread can never stall the pipeline worker.
constexpr std::size_t kAudioRingSamples = 48000 * 2;

/// The RTP clocks the pipeline delivers on: 90 kHz video, 48 kHz audio.
constexpr int kRtpVideoClock = 90000;

} // namespace

PreviewAudioDevice::PreviewAudioDevice(StreamPreview *owner, QObject *parent)
    : QIODevice(parent)
    , m_owner(owner)
{
    open(ReadOnly);
}

qint64 PreviewAudioDevice::readData(char *data, qint64 maxlen)
{
    return m_owner->readFromRing(data, maxlen);
}

StreamPreview::StreamPreview(QObject *parent)
    : QObject(parent)
{
    m_videoDecoder.setFrameCallback([this](AVFrame *frame) { onDecodedVideo(frame); });
    m_videoFilter.setFrameCallback([this](AVFrame *frame) { onFilteredVideo(frame); });
    m_audioDecoder.setFrameCallback([this](AVFrame *frame) { onDecodedAudio(frame); });
}

StreamPreview::~StreamPreview()
{
    // The pipeline always shuts a preview down with its worker joined, but a preview that
    // outlived a sloppy teardown must not leak the audio device here either.
    closeAudioOutput();
}

void StreamPreview::configure(const StreamFormat &format)
{
    std::lock_guard lock(m_formatMutex);
    m_format = format;
}

void StreamPreview::writeVideo(const std::uint8_t *data, std::size_t size,
                               std::uint32_t dtsTimestamp, std::uint32_t ptsTimestamp,
                               bool isKeyframe)
{
    Q_UNUSED(dtsTimestamp) // the decoder's frame timing runs on presentation time

    if (!m_active.load(std::memory_order_relaxed) || size == 0) {
        // No viewer: tear the decoder down if one was left open, so closing the window
        // actually stops the decode. Worker-thread-owned state, safe to touch here.
        if (m_decoderOpen || m_audioDecoderOpen) {
            m_videoFilter.close();
            m_videoDecoder.close();
            m_audioDecoder.close();
            m_decoderOpen = m_filterReady = m_audioDecoderOpen = false;
            m_decoderFailed = m_filterFailed = m_audioDecoderFailed = false;
            m_waitingForKeyframe = true;
        }
        return;
    }

    // A source restart (seek / reconnect) invalidates everything decoded so far.
    if (m_resetPending.exchange(false, std::memory_order_relaxed)) {
        m_videoFilter.close();
        m_videoDecoder.close();
        m_audioDecoder.close();
        m_decoderOpen = m_filterReady = m_audioDecoderOpen = false;
        m_decoderFailed = m_filterFailed = m_audioDecoderFailed = false;
        m_waitingForKeyframe = true;
    }

    if (m_decoderFailed) {
        return; // a broken stream is reported once; retried when the pipeline resets
    }

    if (!m_decoderOpen) {
        QString error;
        if (!m_videoDecoder.open(m_format.videoCodec, &error)) {
            m_decoderFailed = true;
            qWarning("preview: %s", qUtf8Printable(error));
            return;
        }
        m_decoderOpen = true;
        m_waitingForKeyframe = true;
        m_filterReady = m_filterFailed = false;
    }

    // Wait for a keyframe before feeding the decoder: after a seek or reconnect the units
    // arriving here may start mid-GOP, which a decoder must not see.
    if (m_waitingForKeyframe) {
        if (!isKeyframe) {
            return;
        }
        m_waitingForKeyframe = false;
    }

    QString error;
    if (!m_videoDecoder.decode(data, size, std::int64_t(ptsTimestamp), &error)) {
        qWarning("preview: decode failed: %s", qUtf8Printable(error));
    }
}

void StreamPreview::writeAudio(const std::uint8_t *data, std::size_t size,
                               std::uint32_t rtpTimestamp)
{
    Q_UNUSED(rtpTimestamp)

    if (!m_active.load(std::memory_order_relaxed) || size == 0 || m_audioDecoderFailed) {
        return;
    }

    if (!m_audioDecoderOpen) {
        QString error;
        if (!m_audioDecoder.open(m_format.audioCodec, m_format.audioSampleRate,
                                 m_format.audioChannels, m_format.audioExtradata, &error)) {
            m_audioDecoderFailed = true;
            qWarning("preview: audio disabled: %s", qUtf8Printable(error));
            return;
        }
        m_audioDecoderOpen = true;
    }

    QString error;
    m_audioDecoder.decode(data, size, 0, &error); // planar float comes back via callback
}

void StreamPreview::reset()
{
    // May be called from either thread (the pipeline's reconnect timer runs on the GUI
    // thread, the seek hook on the worker). The worker applies the teardown at the next
    // writeVideo, which is the only place the decoder state is ever touched.
    m_resetPending.store(true, std::memory_order_relaxed);
    m_hasFrame.store(false, std::memory_order_relaxed);
    Q_EMIT framesStarted(); // the placeholder comes back until the new generation's first frame

    std::lock_guard lock(m_audioMutex);
    m_audioRing.clear();
}

void StreamPreview::shutdown()
{
    m_videoFilter.close();
    m_videoDecoder.close();
    m_audioDecoder.close();
    m_decoderOpen = m_filterReady = m_audioDecoderOpen = false;
    m_decoderFailed = m_filterFailed = m_audioDecoderFailed = false;
    m_waitingForKeyframe = true;
    closeAudioOutput();
}

void StreamPreview::attachTo(QVideoSink *sink)
{
    // GUI thread. The sink belongs to the caller's VideoOutput and outlives this call while
    // the window is open; detach() runs from the window's onVisibleChanged before the item
    // can be destroyed.
    m_targetSink.store(sink, std::memory_order_relaxed);
    setActive(sink != nullptr);
}

void StreamPreview::detach()
{
    m_targetSink.store(nullptr, std::memory_order_relaxed);
    setActive(false);
}

void StreamPreview::setActive(bool active)
{
    if (m_active.exchange(active, std::memory_order_relaxed) == active) {
        return;
    }

    if (active) {
        // The worker re-arms its keyframe gate through the pending reset: the first frame
        // shown after attaching is always the next keyframe, never a mid-GOP unit.
        m_resetPending.store(true, std::memory_order_relaxed);
        m_hasFrame.store(false, std::memory_order_relaxed);
        openAudioOutput();
    } else {
        closeAudioOutput();
    }

    Q_EMIT activeChanged();
}

void StreamPreview::setMuted(bool muted)
{
    if (m_muted.exchange(muted, std::memory_order_relaxed) == muted) {
        return;
    }

    // The audio sink lives on the GUI thread, which owns setActive() too, so stopping it
    // here is safe. Unmuting restarts it from the ring's current head.
    if (muted) {
        closeAudioOutput();
    } else if (m_active.load(std::memory_order_relaxed)) {
        openAudioOutput();
    }

    Q_EMIT mutedChanged();
}

void StreamPreview::onDecodedVideo(AVFrame *frame)
{
    if (!m_active.load(std::memory_order_relaxed)) {
        return;
    }

    if (!m_filterReady) {
        if (m_filterFailed) {
            return;
        }
        // Cap the preview size so a 4K source does not balloon the viewer's texture; the
        // filter keeps the aspect ratio when only one dimension is given.
        int width = 0;
        int height = 0;
        if (frame->width > kPreviewMaxWidth || frame->height > kPreviewMaxHeight) {
            const double scale = std::min(double(kPreviewMaxWidth) / frame->width,
                                          double(kPreviewMaxHeight) / frame->height);
            width = int(frame->width * scale) & ~1;
            height = int(frame->height * scale) & ~1;
        }

        QString error;
        if (!m_videoFilter.open(frame, AVRational { 1, kRtpVideoClock }, width, height,
                                kPreviewFpsNum, kPreviewFpsDen, AV_PIX_FMT_BGR0, &error)) {
            m_filterFailed = true;
            qWarning("preview: %s", qUtf8Printable(error));
            return;
        }
        m_filterReady = true;
    }

    QString error;
    if (!m_videoFilter.push(frame, &error)) {
        qWarning("preview: filter failed: %s", qUtf8Printable(error));
    }
}

void StreamPreview::onFilteredVideo(AVFrame *frame)
{
    if (!m_active.load(std::memory_order_relaxed)) {
        return;
    }

    const int stride = frame->width * 4; // BGR0 = 32 bpp

    // Build the QImage over the filter's padded output and immediately copy it: copy()
    // detaches into a buffer the QImage (and through it the QVideoFrame) owns, so nothing
    // here aliases the filter's reusable output frame.
    QImage wrapped(reinterpret_cast<const uchar *>(frame->data[0]), frame->width,
                   frame->height, stride, QImage::Format_RGB32);
    const QImage image = wrapped.copy();

    QVideoFrame videoFrame(image);
    if (!videoFrame.isValid()) {
        return;
    }

    const bool first = !m_hasFrame.exchange(true, std::memory_order_relaxed);
    m_frameWidth = frame->width;
    m_frameHeight = frame->height;

    // QVideoSink::setVideoFrame is reentrant: the owning VideoOutput receives the frame on
    // the GUI thread through a queued connection, so pushing from the pipeline worker is the
    // supported pattern (it is what QMediaPlayer does with its own output sink).
    if (QVideoSink *sink = m_targetSink.load(std::memory_order_relaxed)) {
        sink->setVideoFrame(videoFrame);
    }

    if (first) {
        Q_EMIT framesStarted();
    }
}

void StreamPreview::onDecodedAudio(AVFrame *frame)
{
    if (!m_active.load(std::memory_order_relaxed) || m_muted) {
        return;
    }

    // AudioDecoder guarantees planar float (AV_SAMPLE_FMT_FLTP). Interleave into the ring;
    // mono is duplicated so the ring always holds stereo, matching the sink's format.
    const int channels = std::max(1, frame->ch_layout.nb_channels);
    const int samples = frame->nb_samples;
    if (samples <= 0 || !frame->data[0] || !frame->extended_data) {
        return;
    }

    std::lock_guard lock(m_audioMutex);
    const std::size_t room = kAudioRingSamples - std::min(kAudioRingSamples, m_audioRing.size());
    const std::size_t incoming = std::size_t(samples) * 2;
    if (incoming > room) {
        // Consumer stalled: shed the oldest audio rather than blocking the pipeline.
        m_audioRing.erase(m_audioRing.begin(),
                          m_audioRing.begin() + std::min(m_audioRing.size(), incoming - room));
    }

    for (int s = 0; s < samples; ++s) {
        const float left = reinterpret_cast<const float *>(frame->extended_data[0])[s];
        const float right = channels > 1
            ? reinterpret_cast<const float *>(frame->extended_data[1])[s]
            : left;
        m_audioRing.push_back(left);
        m_audioRing.push_back(right);
    }
}

void StreamPreview::openAudioOutput()
{
    if (m_audioSink || m_muted) {
        return;
    }

    StreamFormat format;
    {
        std::lock_guard lock(m_formatMutex);
        format = m_format;
    }
    if (!format.hasAudio || format.audioSampleRate <= 0) {
        return; // no audio to play (or the format is not negotiated yet)
    }

    // The ring always holds interleaved stereo float at the source's sample rate (mono is
    // duplicated on the way in), so the sink format must match it exactly.
    QAudioFormat audioFormat;
    audioFormat.setChannelCount(2);
    audioFormat.setSampleRate(format.audioSampleRate);
    audioFormat.setSampleFormat(QAudioFormat::Float);

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (!device.isFormatSupported(audioFormat)) {
        qInfo("preview: default audio output does not support %d Hz float stereo; audio "
              "muted for this viewer",
              audioFormat.sampleRate());
        return;
    }

    m_audioDevice = new PreviewAudioDevice(this, this);
    m_audioSink = new QAudioSink(device, audioFormat, this);
    m_audioSink->start(m_audioDevice);
}

void StreamPreview::closeAudioOutput()
{
    if (m_audioSink) {
        m_audioSink->stop();
        m_audioSink->deleteLater();
        m_audioSink = nullptr;
    }
    if (m_audioDevice) {
        m_audioDevice->deleteLater();
        m_audioDevice = nullptr;
    }
    std::lock_guard lock(m_audioMutex);
    m_audioRing.clear();
}

qint64 StreamPreview::readFromRing(char *data, qint64 bytes)
{
    std::lock_guard lock(m_audioMutex);
    const qint64 samplesNeeded = bytes / qint64(sizeof(float));
    qint64 filled = 0;
    while (filled < samplesNeeded && !m_audioRing.empty()) {
        const float sample = m_audioRing.front();
        m_audioRing.pop_front();
        std::memcpy(data + filled * qint64(sizeof(float)), &sample, sizeof(float));
        ++filled;
    }
    if (filled < samplesNeeded) {
        // Silence-pad an underrun so the device never sees a short read as an error.
        std::memset(data + filled * qint64(sizeof(float)), 0,
                    std::size_t(samplesNeeded - filled) * sizeof(float));
        filled = samplesNeeded;
    }
    return filled * qint64(sizeof(float));
}

} // namespace CBridge
