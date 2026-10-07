/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "core/bridgetypes.h"
#include "media/audiodecoder.h"
#include "media/videodecoder.h"
#include "media/videofilter.h"
#include "sinks/streamsink.h" // StreamFormat

#include <QAudioFormat>
#include <QByteArray>
#include <QIODevice>
#include <QObject>
#include <QString>
#include <QVideoSink> // full type: the videoSink Q_PROPERTY needs its metatype

#include <cstdint>
#include <atomic>
#include <deque>
#include <mutex>

class QAudioSink;

namespace CBridge {

class StreamPreview;

/// Pull-mode QIODevice handed to QAudioSink: Qt's audio thread calls readData(), which
/// drains the ring the preview's worker thread fills. Returns silence when the ring is
/// empty so the device never underruns into a stall.
class PreviewAudioDevice : public QIODevice
{
    Q_OBJECT

public:
    explicit PreviewAudioDevice(StreamPreview *owner, QObject *parent = nullptr);

protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *, qint64) override { return 0; } // read-only device

private:
    StreamPreview *m_owner;
};

/// One stream's built-in viewer: decodes the stream's own bitstream for display and plays
/// its audio locally, without touching the sinks.
///
/// The pipeline drives it from its worker thread exactly like a sink (writeVideo/writeAudio
/// with the same Annex-B access units and Opus packets the sinks receive), but nothing here
/// runs until a viewer attaches: setActive(true) arms the decoder gate, and frames are only
/// produced while active. Decoding reuses the production VideoDecoder (NVDEC when available)
/// and VideoFilter; the filter's last stage converts to BGR0, which is handed to a
/// QVideoSink as a QImage. QVideoSink is reentrant by design — the worker thread pushes
/// frames while the GUI thread's VideoOutput receives them through a queued connection, so
/// no manual marshalling is needed here.
///
/// Audio takes the same route as the NDI sink's AudioDecoder (Opus to planar float), but is
/// interleaved into a small ring buffer that a QAudioSink pulls from its own audio thread.
/// The sink exists only while active; the ring sheds old data when the consumer stalls so
/// the worker thread never blocks.
class StreamPreview : public QObject
{
    Q_OBJECT

    /// True once at least one frame has been rendered since the last reset.
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY framesStarted)

    /// True while a viewer is attached and frames are being produced.
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

    /// Local audio playback toggle for the viewer window.
    Q_PROPERTY(bool muted READ isMuted WRITE setMuted NOTIFY mutedChanged)

public:
    explicit StreamPreview(QObject *parent = nullptr);
    ~StreamPreview() override;

    /// The negotiated media format, pushed by the pipeline whenever its sinks open. Safe
    /// from the worker thread: it only stores a copy the audio path reads under a mutex.
    void configure(const StreamFormat &format);

    /// Driven by the pipeline's worker thread, once per access unit / audio packet. No-ops
    /// while inactive, so an unwatched stream pays nothing beyond the queue copy the sinks
    /// already make.
    void writeVideo(const std::uint8_t *data, std::size_t size, std::uint32_t dtsTimestamp,
                    std::uint32_t ptsTimestamp, bool isKeyframe);
    void writeAudio(const std::uint8_t *data, std::size_t size, std::uint32_t rtpTimestamp);

    /// Called by the pipeline when the source restarted its timeline (a seeked yt-dlp child)
    /// or reconnected: drops the decoder and the audio ring so the preview waits for the new
    /// generation's first keyframe, exactly like the sinks do. Safe from any thread: the
    /// worker re-opens the decoder on the next frame when a viewer is attached.
    void reset();

    /// Tear everything down. The pipeline calls this from stop() after its worker thread has
    /// been joined, which is the only moment the decoder/filter (worker-thread-owned) may be
    /// closed from the GUI thread.
    void shutdown();

    /// Attach/detach the viewer window. Called from the GUI thread.
    ///
    /// QQuickVideoOutput owns its QVideoSink and exposes it read-only, so the viewer hands
    /// over that sink and this object pushes frames into it — the same thing QMediaPlayer
    /// does internally. setVideoFrame is reentrant, which is what makes pushing from the
    /// pipeline's worker thread legal. Attaching also arms decoding.
    Q_INVOKABLE void attachTo(QVideoSink *sink);
    Q_INVOKABLE void detach();

    void setActive(bool active);
    bool isActive() const { return m_active.load(std::memory_order_relaxed); }

    bool hasFrame() const { return m_hasFrame.load(std::memory_order_relaxed); }

    void setMuted(bool muted);
    bool isMuted() const { return m_muted.load(std::memory_order_relaxed); }

Q_SIGNALS:
    void framesStarted();
    void activeChanged();
    void mutedChanged();

private:
    friend class PreviewAudioDevice;
    friend struct PreviewFrameTestAccess;

    void onDecodedVideo(AVFrame *frame);
    void onFilteredVideo(AVFrame *frame);
    void onDecodedAudio(AVFrame *frame);

    /// GUI thread: build the QAudioSink for the configured format. Silent no-op when the
    /// default output device cannot be opened.
    void openAudioOutput();
    void closeAudioOutput();

    /// Audio-thread callback used by PreviewAudioDevice: pops interleaved float samples from
    /// the ring, padding with silence when it runs dry.
    qint64 readFromRing(char *data, qint64 bytes);

    StreamFormat m_format; // guarded by m_formatMutex (worker writes, GUI reads)
    std::mutex m_formatMutex;

    VideoDecoder m_videoDecoder;
    VideoFilter m_videoFilter;
    // Decoder/filter/flag state below belongs to the pipeline's worker thread: writeVideo()
    // opens the decoder the first time a frame arrives while a viewer is active and closes
    // it again once the viewer detaches, so setActive() never has to touch it. shutdown()
    // (GUI thread, after the worker was joined) is the only other closer.
    bool m_decoderOpen = false;
    bool m_decoderFailed = false;     // retry at the next keyframe
    bool m_filterReady = false;
    bool m_filterFailed = false;
    bool m_waitingForKeyframe = true;

    AudioDecoder m_audioDecoder;
    bool m_audioDecoderOpen = false;
    bool m_audioDecoderFailed = false;

    /// Where frames go while a viewer is attached. Written on the GUI thread, read by the
    /// pipeline's worker thread when it publishes a frame.
    std::atomic<QVideoSink *> m_targetSink { nullptr };
    std::atomic_bool m_active { false };
    std::atomic_bool m_resetPending { false }; // set by reset(), applied by the worker
    std::atomic_bool m_hasFrame { false };
    std::atomic_bool m_muted { false };

    int m_frameWidth = 0;
    int m_frameHeight = 0;

    QAudioSink *m_audioSink = nullptr; // GUI thread; replaced on every activation
    PreviewAudioDevice *m_audioDevice = nullptr;
    /// Interleaved float PCM produced by the worker thread, consumed by the audio thread.
    /// Bounded: the producer drops the oldest samples rather than blocking the pipeline.
    std::deque<float> m_audioRing;
    std::mutex m_audioMutex;
};

} // namespace CBridge
