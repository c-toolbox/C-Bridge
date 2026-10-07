/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "config/bridgeconfig.h"
#include "core/streamsource.h"
#include "ytdlp/audiotranscoder.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

// FFmpeg types used by the direct-URL path's helpers; the .cpp includes the real headers.
struct AVFormatContext;

namespace CBridge {

/// Pulls a YouTube video through yt-dlp and feeds the pipeline like any other source.
///
/// A child `yt-dlp -f <video+audio> --downloader ffmpeg -o - <url>` writes one interleaved media stream to stdout,
/// redirected into an anonymous Win32 pipe; the worker thread demuxes that pipe with
/// libavformat through a custom AVIOContext whose read callback pulls from the pipe with a
/// short peek timeout and checks the stop flag — the same interruptible-read contract
/// SrtSource achieves with its interrupt callback.
///
/// FFmpeg merges independent inputs into pipe-safe MPEG-TS. H.264/HEVC pass through;
/// VP9/AV1 and HDR convert to SDR H.264 at the source resolution (NVENC when usable,
/// libx264 otherwise). When every enabled sink decodes the video itself (NDI), SDR VP9/AV1
/// skip that conversion and pass through in Matroska instead. Audio becomes Opus 48 kHz
/// stereo. The direct reader remains
/// available for selected bundled HLS streams; its AAC audio uses AudioTranscoder.
class YtdlpSource : public StreamSource
{
    Q_OBJECT

public:
    explicit YtdlpSource(QObject *parent = nullptr);
    ~YtdlpSource() override;

    void setConfig(const StreamConfig &config) override;

    void start() override;
    void stop() override;

    VideoCodec negotiatedVideoCodec() const override;
    AudioCodec negotiatedAudioCodec() const override;
    int negotiatedAudioSampleRate() const override;
    int negotiatedAudioChannels() const override;

    /// True once the metadata probe has run and the feed is not live: pause/resume work for
    /// both live and VOD, but seek only makes sense with a finite duration.
    bool isPlaybackControllable() const override;
    bool isLive() const override;

    /// Stops draining the stdout pipe. yt-dlp blocks once the ~1 MiB pipe fills, so the child
    /// stalls on OS backpressure with no bytes lost and no process churn (see plan §4.5).
    void requestPause() override;
    void requestResume() override;

    /// Restarts the merge child at `positionMs` using FFmpeg's output seek. Ignored (once, with an
    /// error) for live feeds, which have no seekable timeline.
    void requestSeek(qint64 positionMs) override;

    double mediaPositionSeconds() const override;
    double mediaDurationSeconds() const override;

    bool takeGenerationRestartPending() override;

    /// The source's last notable recovery event for the UI's stats row: which fetch path
    /// is live (direct HLS vs yt-dlp pipe), a stall resume, or a fallback. See §2–§4 of
    /// YTDLP_IMPROVEMENT_PLAN.md.
    QString lastEvent() const override;

private:
    /// Worker thread body. Resolves yt-dlp, probes the metadata, then runs child generations
    /// until a stop, an error or the end of the feed. A seek starts a new generation instead
    /// of ending the run.
    bool run();

    /// How a child generation ended.
    enum class GenerationResult {
        Stopped, ///< stop() was requested: the run is over
        Seek,    ///< a seek was requested: respawn at the new position
        Ended,   ///< the pipe closed cleanly (end of file or end of live)
        Failed,  ///< an error occurred; `error` describes it
    };

    /// One yt-dlp child generation: spawn behind the job object, demux the stdout pipe and
    /// pump packets. `startSec` > 0 asks yt-dlp for that section only (the seek path).
    /// Sets m_lastGenerationDelivered when at least one video packet reached the pipeline,
    /// which is what makes a later failure eligible for an in-place resume.
    bool spawnAndPump(const QString &program, double startSec, GenerationResult *result,
                      QString *error);

    /// Direct-URL generation (plan §4): FFmpeg's HLS reader fetches the resolved muxed
    /// playlist itself — no child process, no pipe. Internally reconnecting, so transient
    /// network errors never reach the pipeline. `startSec` > 0 seeks after opening. Seeks
    /// during the run are served in place (av_seek_frame), not by a new generation. The
    /// caller distinguishes a pre-delivery failure (fall back to the pipe) from a
    /// mid-stream one by m_lastGenerationDelivered.
    void spawnDirect(double startSec, GenerationResult *result, QString *error);

    /// Shared pump loop for both fetch paths: dispatch packets to the callbacks until stop,
    /// seek, EOF or error. `format` must be open with streams identified. With
    /// `nativeSeek` (the direct path) a seek request is served by av_seek_frame inside the
    /// loop; otherwise the loop returns Seek and run() respawns the child at the target.
    void pumpPackets(AVFormatContext *format, int videoIndex, int audioIndex,
                     bool transcodingAudio, double startSec, bool nativeSeek,
                     GenerationResult *result, QString *error);

    /// Open the resolved direct URL with FFmpeg's HLS reader (reconnect + persistent
    /// connections + the format's own headers). On success fills the demuxer context and
    /// the stream indices; the caller owns `*format` afterwards.
    bool openDirectFormat(AVFormatContext **format, int *videoIndex, int *audioIndex,
                          bool *transcodingAudio, QString *error);

    /// One-shot `yt-dlp -J` preflight: fills in the duration and live flag that drive the UI's
    /// seek slider, the selected codec/conversion requirements and the direct-URL/headers
    /// for compatible bundled HLS. A failed probe prevents unsafe codec passthrough.
    void probeMetadata(const QString &program);

    /// AVIOContext read callback: pulls from the stdout pipe with a short peek timeout,
    /// reports a stalled pipe (no bytes for 30 s) as ETIMEDOUT and EOF once yt-dlp exits.
    static int pipeReadCallback(void *opaque, std::uint8_t *buf, int bufSize);

    /// FFmpeg interrupt callback for the direct path: aborts a blocking network read when
    /// stop or seek was requested, the same contract the pipe read callback provides.
    static int directInterruptCallback(void *opaque);

    /// Resolution order: per-stream path, app-wide KConfig path, PATH lookup, the machine
    /// fallback D:/FFmpeg/yt-dlp.exe. Empty string + *error when nothing is found.
    QString resolveYtDlpPath(QString *error) const;

    /// Quotes one argument for a CreateProcessW command line (MSVCRT rules).
    static QString quoteArgument(const QString &argument);

    /// Appends the tail of yt-dlp's stderr to `cause` and adds an actionable hint when it
    /// matches a known failure mode (bot check, no matching format, missing JS runtime).
    QString describeFailure(const QString &cause) const;

    /// Terminates every member of the job object (the PyInstaller bootloader and the real
    /// Python child) and waits for the process to exit. Worker thread only; a no-op when no
    /// process was spawned.
    void killChild();

    /// Drains the stderr pipe until EOF so yt-dlp never blocks on a full pipe buffer.
    void drainStderr();

    /// The shared yt-dlp argument prefix: config isolation, retries and the pinned ffmpeg.
    /// `jsonProbe` switches to the machine-readable metadata form used by probeMetadata().
    QStringList baseArguments(bool jsonProbe, double startSec = 0.0) const;

    /// Records a one-line recovery event for the UI ("resumed at 12.3 s after stall", …).
    /// Worker thread; read by the GUI through lastEvent().
    void setLastEvent(const QString &event);

    /// True when the direct-URL path is available for the next generation: the mode is not
    /// "off", the probe found a usable muxed HLS URL matching the selector, and no
    /// earlier attempt gave up.
    bool directEligible() const;

    YouTubeSourceConfig m_config;
    /// Every enabled sink decodes (NDI), so no sink needs H.264/H.265 on the wire.
    bool m_decodingSinksOnly = false;
    bool m_formatResolved = false;
    QString m_probeError;
    QString m_selectedVideoCodec;
    QString m_selectedDynamicRange;
    bool m_separateInputs = false;
    bool m_useNvenc = false;
    QString m_ffmpegProgram;
    QString m_ytdlpProgram;
    QString m_pathError;
    int m_initialPipeStallSeconds = 30;

    std::thread m_thread;
    std::atomic<bool> m_stop { false };

    std::atomic<VideoCodec> m_videoCodec { VideoCodec::Unknown };
    std::atomic<AudioCodec> m_audioCodec { AudioCodec::Unknown };
    std::atomic<int> m_audioSampleRate { 48000 };
    std::atomic<int> m_audioChannels { 2 };

    // --- Playback control (see YTDLP_SOURCE_PLAN.md §4.5) -------------------------------
    // Written from the GUI thread, read by the worker's pump loop. Plain flags: a command
    // never has to synchronise with the read loop, which polls them between packets.
    std::atomic<bool> m_paused { false };
    /// Absolute seek target in ms; -1 means "no seek pending".
    std::atomic<qint64> m_seekToMs { -1 };
    std::atomic<bool> m_seekRejected { false };

    /// Media clock for the UI, polled by the engine's 1 Hz stats sample. The pump loop stores
    /// the generation's anchor plus the delivered PTS relative to its first packet, so a seek
    /// keeps the playhead continuous across the child restart.
    std::atomic<double> m_positionSec { 0.0 };
    std::atomic<double> m_durationSec { 0.0 };
    std::atomic<bool> m_live { false };
    std::atomic<bool> m_probed { false }; // the -J metadata probe has completed (success or not)

    /// Set when a new child generation starts and taken by the pipeline's video callback, so
    /// the pipeline can re-arm its keyframe gate and see a clean restart like a reconnect.
    std::atomic<bool> m_freshGeneration { false };

    // --- Resilience (see YTDLP_IMPROVEMENT_PLAN.md §2–§4) --------------------------------
    /// Where the next run() should start: -1 = from zero, >= 0 = resume after a mid-stream
    /// failure. Set by the worker just before reporting Failed so the pipeline's reconnect
    /// restarts at the playhead instead of at zero; cleared by a user-initiated stop.
    std::atomic<double> m_pendingResumeSec { -1.0 };

    /// True once at least one video packet of the current generation reached the pipeline.
    /// A failure after that is a mid-stream stall (resumable); before it, the generation
    /// never really started (fall back / fail cleanly instead).
    bool m_lastGenerationDelivered = false;

    /// Consecutive in-source stall resumes without a healthy window between them. Reset by
    /// any generation that ran clean for kHealthyWindowSeconds; capped by kMaxStallRetries,
    /// after which the source reports Failed and the pipeline's backoff takes over.
    int m_stallRetries = 0;

    /// When the current generation delivered its first video packet: a generation that
    /// then ran clean for kHealthyWindowSeconds resets the stall-retry budget.
    std::chrono::steady_clock::time_point m_deliveryStartTime {};

    /// True while the current generation fetches through FFmpeg's HLS reader instead of the
    /// yt-dlp stdout pipe. Worker-thread state; lastEvent() mirrors it for the UI.
    bool m_directActive = false;

    /// True while pumpPackets is inside av_read_frame on the direct path: the interrupt
    /// callback may abort a blocking read for a pending seek only then — interrupting the
    /// initial avformat_open_input for a seek would turn a user action into a feed error.
    std::atomic<bool> m_pumpActive { false };

    /// The resolved direct URL and its required request headers (from the -J probe's chosen
    /// muxed-HLS format). Worker-thread only: probe and generations run in sequence there.
    QString m_directUrl;
    QString m_directHeaders;

    /// Direct-path attempt bookkeeping: one re-probe is allowed after a failure (YouTube's
    /// signed URLs expire, so a stale URL deserves fresh resolution), then the source
    /// commits to the pipe path until the next user-initiated restart.
    int m_directAttempts = 0;
    bool m_directGaveUp = false;

    mutable std::mutex m_eventMutex;
    QString m_lastEventString;

    // Win32 process and pipes, owned by the worker thread for its whole lifetime. Stored as
    // void* so this header stays free of <windows.h>; cast back to HANDLE in the .cpp.
    void *m_process = nullptr;
    void *m_job = nullptr;
    void *m_stdoutRead = nullptr;
    void *m_stderrRead = nullptr;

    bool m_assignedToJob = false; // CreateProcess succeeded and AssignProcessToJobObject did too
    std::uint32_t m_exitCode = 0;
    bool m_exitCodeValid = false;

    std::thread m_stderrThread;
    mutable std::mutex m_stderrMutex;
    QByteArray m_stderrTail; // last few KB of yt-dlp's stderr, for error messages

    /// Steady-clock time of the last byte on the stdout pipe (stall detection). Touched only
    /// from the worker thread.
    std::chrono::steady_clock::time_point m_lastByteTime {};

    AudioTranscoder m_transcoder;
};

} // namespace CBridge
