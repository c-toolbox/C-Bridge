/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ytdlp/ytdlpsource.h"
#include "ytdlp/playbackclock.h"
#include "ytdlp/ffmpegstreamoptions.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include "cbridgesettings.h"

#include <QFile>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#include <windows.h>

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <vector>

// Directory holding the pinned ffmpeg.exe (set by CMake from FFMPEG_RUNTIME_DIR). yt-dlp's
// --ffmpeg-location must point at the full executable on this layout, so the source appends
// /ffmpeg.exe and checks for its existence before passing it. Empty when undefined: the flag
// is simply omitted and format merging is left to yt-dlp's own discovery.
#ifndef CBRIDGE_FFMPEG_BIN_DIR
#define CBRIDGE_FFMPEG_BIN_DIR ""
#endif

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {

/// No bytes on the stdout pipe for this long means the feed is stalled (network hiccup, a
/// bot-check page that produced no media, ...). The read callback reports ETIMEDOUT and the
/// source retries in place (respawn the child at the playhead) before ever reporting
/// Failed, so a hiccup never reaches the pipeline's reconnect logic (plan §2).
constexpr int kPipeStallSeconds = 30;

/// How many consecutive in-place stall resumes are allowed before the source gives up and
/// lets the pipeline's backoff take over. A generation that delivers media for longer than
/// kHealthyWindowSeconds resets the counter.
constexpr int kMaxStallRetries = 3;
constexpr int kHealthyWindowSeconds = 30;

/// A resume this close to the start is indistinguishable from a fresh start, and resuming
/// there on a URL that fails instantly would just loop. Below this, start from zero.
constexpr double kResumeFloorSeconds = 2.0;

/// How long stop() waits for yt-dlp to exit after terminating it. Bounded so a wedged child
/// can never hang shutdown.
constexpr DWORD kKillWaitMs = 5000;

/// How often the pump loop re-checks the pause/seek/stop flags while blocked in a read, and
/// how long a paused loop waits between checks. The pipe read itself blocks up to
/// kPipeStallSeconds, so the loop also breaks the read at this cadence by polling first.
constexpr DWORD kPumpPollMs = 20;

QString resolveFfmpegProgram()
{
    const QString pinned = QLatin1String(CBRIDGE_FFMPEG_BIN_DIR) + u"/ffmpeg.exe"_s;
    if (QFile::exists(pinned)) return pinned;
    const QString bundled = QCoreApplication::applicationDirPath() + u"/ffmpeg.exe"_s;
    if (QFile::exists(bundled)) return bundled;
    return QStandardPaths::findExecutable(u"ffmpeg"_s);
}

// Try an actual frame rather than trusting -encoders: NVENC can be compiled in
// without a usable GPU/driver. Software encoding is the portable fallback.
bool usableNvenc(const QString &program, const std::atomic<bool> &stop)
{
    QProcess check;
    check.start(program, {u"-hide_banner"_s, u"-loglevel"_s, u"error"_s,
        u"-f"_s, u"lavfi"_s, u"-i"_s, u"color=s=640x360:d=0.1"_s,
        u"-frames:v"_s, u"1"_s, u"-c:v"_s, u"h264_nvenc"_s, u"-f"_s, u"null"_s, u"-"_s});
    for (int waited = 0; waited < 5000 && !stop.load(std::memory_order_relaxed); waited += 100) {
        if (check.waitForFinished(100))
            return check.exitStatus() == QProcess::NormalExit && check.exitCode() == 0;
        check.readAllStandardError();
    }
    check.kill();
    check.waitForFinished(1000);
    return false;
}

/// FFmpeg's `headers` option form: "Key: value\r\n" per line. Accept-Encoding is dropped —
/// YouTube's CDN answers gzip only when the client also handles it, and FFmpeg negotiates
/// that itself; forwarding yt-dlp's value would ask for a codec FFmpeg's HTTP stack may not
/// run here.
QString buildHeaderString(const QJsonObject &headers)
{
    QString out;
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        if (it.key().compare(u"Accept-Encoding"_s, Qt::CaseInsensitive) == 0) {
            continue;
        }
        const QString value = it.value().toString();
        if (!value.isEmpty()) {
            out += it.key() + u": "_s + value + u"\r\n"_s;
        }
    }
    return out;
}

} // namespace

YtdlpSource::YtdlpSource(QObject *parent)
    : StreamSource(parent)
{
}

YtdlpSource::~YtdlpSource()
{
    stop();
}

void YtdlpSource::setConfig(const StreamConfig &config)
{
    m_config = config.youtube;
    m_decodingSinksOnly = false;
    for (const SinkConfig &sink : config.sinks) {
        if (!sink.enabled) {
            continue;
        }
        if (sink.kind != SinkKind::Ndi) {
            m_decodingSinksOnly = false;
            break;
        }
        m_decodingSinksOnly = true;
    }
}

VideoCodec YtdlpSource::negotiatedVideoCodec() const
{
    return m_videoCodec.load(std::memory_order_relaxed);
}

AudioCodec YtdlpSource::negotiatedAudioCodec() const
{
    return m_audioCodec.load(std::memory_order_relaxed);
}

int YtdlpSource::negotiatedAudioSampleRate() const
{
    return m_audioSampleRate.load(std::memory_order_relaxed);
}

int YtdlpSource::negotiatedAudioChannels() const
{
    return m_audioChannels.load(std::memory_order_relaxed);
}

void YtdlpSource::start()
{
    // The pipeline restarts a Failed source by calling start() again with the worker thread
    // already finished (its run() returned after reporting Failed). Join that dead thread
    // here *without* clearing the resilience state: the resume position, the stall-retry
    // counter and the direct-path bookkeeping are exactly what a reconnect must carry over
    // so a hiccup resumes at the playhead instead of at zero (plan §2).
    m_stop.store(false, std::memory_order_relaxed);
    if (m_thread.joinable()) {
        m_stop.store(true, std::memory_order_relaxed);
        m_paused.store(false, std::memory_order_relaxed);
        m_thread.join();
        m_stop.store(false, std::memory_order_relaxed);
    }
    m_paused.store(false, std::memory_order_relaxed);
    m_seekToMs.store(-1, std::memory_order_relaxed);
    m_seekRejected.store(false, std::memory_order_relaxed);
    m_assignedToJob = false;
    m_exitCodeValid = false;
    // KConfig's generated singleton is not safe to initialise concurrently from
    // source workers. Resolve settings on the owning thread before launching one.
    m_pathError.clear();
    m_ytdlpProgram = resolveYtDlpPath(&m_pathError);
    m_thread = std::thread([this] { run(); });
}

void YtdlpSource::stop()
{
    m_stop.store(true, std::memory_order_relaxed);
    m_paused.store(false, std::memory_order_relaxed); // let a paused pump loop reach the stop check
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // A user-initiated stop is a clean slate: the next start replays from zero, re-probes
    // (the video identity may have changed) and retries the direct path from scratch. The
    // reconnect path never reaches here — the pipeline's timer calls start() directly.
    m_pendingResumeSec.store(-1.0, std::memory_order_relaxed);
    m_stallRetries = 0;
    m_directAttempts = 0;
    m_directGaveUp = false;
    m_probed.store(false, std::memory_order_relaxed);
    m_directUrl.clear();
    m_directHeaders.clear();
    m_stderrTail.clear(); // safe: the stderr drain thread was joined by the run above
}

bool YtdlpSource::isPlaybackControllable() const
{
    // Controllable once the probe has answered and the feed is not live: pause/resume work
    // for live too, but the UI groups the transport row under "controllable", and seek — the
    // main reason to show it — is meaningless for a live stream.
    return m_probed.load(std::memory_order_relaxed)
        && !m_live.load(std::memory_order_relaxed);
}

bool YtdlpSource::isLive() const
{
    return m_live.load(std::memory_order_relaxed);
}

void YtdlpSource::requestPause()
{
    if (state() != StreamState::Running) {
        return;
    }
    if (!m_paused.exchange(true, std::memory_order_relaxed)) {
        setState(StreamState::Paused);
    }
}

void YtdlpSource::requestResume()
{
    if (m_paused.exchange(false, std::memory_order_relaxed)) {
        setState(StreamState::Running);
    }
}

void YtdlpSource::requestSeek(qint64 positionMs)
{
    if (m_live.load(std::memory_order_relaxed)) {
        // Live has no seekable timeline. Report it once per run so a stuck control does not
        // spam the status bar.
        if (!m_seekRejected.exchange(true, std::memory_order_relaxed)) {
            Q_EMIT errorOccurred(u"Seek is not available for live streams"_s);
        }
        return;
    }
    if (positionMs < 0) {
        positionMs = 0;
    }
    const double duration = m_durationSec.load(std::memory_order_relaxed);
    if (duration > 0.0 && positionMs > qint64(duration * 1000.0)) {
        positionMs = qint64(duration * 1000.0);
    }

    // The pump loop notices the flag, tears the child down and run() respawns it at the new
    // position. A paused stream stays paused across the seek: the new generation starts with
    // the pump gate closed, so the playhead sits at the target until the user resumes.
    m_seekToMs.store(positionMs, std::memory_order_relaxed);
}

double YtdlpSource::mediaPositionSeconds() const
{
    return m_positionSec.load(std::memory_order_relaxed);
}

double YtdlpSource::mediaDurationSeconds() const
{
    return m_durationSec.load(std::memory_order_relaxed);
}

bool YtdlpSource::takeGenerationRestartPending()
{
    return m_freshGeneration.exchange(false, std::memory_order_relaxed);
}

QString YtdlpSource::lastEvent() const
{
    std::lock_guard lock(m_eventMutex);
    return m_lastEventString;
}

void YtdlpSource::setLastEvent(const QString &event)
{
    std::lock_guard lock(m_eventMutex);
    m_lastEventString = event;
}

bool YtdlpSource::directEligible() const
{
    if (m_config.directUrlMode == u"off"_s || m_directGaveUp) {
        return false;
    }
    return !m_directUrl.isEmpty();
}

QString YtdlpSource::resolveYtDlpPath(QString *error) const
{
    // 1. Per-stream path from the configuration document. An explicit value that does not
    //    exist is a hard error: silently falling back would hide a broken setting.
    const QString perStream = m_config.ytDlpPath.trimmed();
    if (!perStream.isEmpty()) {
        if (QFile::exists(perStream)) {
            return perStream;
        }
        if (error) {
            *error = u"The configured yt-dlp path does not exist: %1"_s.arg(perStream);
        }
        return {};
    }

    // 2. App-wide KConfig preference (Settings → yt-dlp). A stale value falls through to the
    //    next candidate instead of failing, since it may point at another machine's layout.
    const QString configured = CBridgeSettings::self()->ytdlpPath();
    if (!configured.trimmed().isEmpty() && QFile::exists(configured)) {
        return configured;
    }

    // 3. PATH lookup (works for a yt-dlp installed via pip/winget/scoop).
    const QString onPath = QStandardPaths::findExecutable(u"yt-dlp"_s);
    if (!onPath.isEmpty()) {
        return onPath;
    }

    // 4. Machine fallback, matching the build machines' layout (see YTDLP_SOURCE_PLAN.md §3).
    static const QString kFallback = u"D:/FFmpeg/yt-dlp.exe"_s;
    if (QFile::exists(kFallback)) {
        return kFallback;
    }

    if (error) {
        *error = u"yt-dlp not found — set its path in Settings or on the stream, or install it "
                 u"(winget install yt-dlp.yt-dlp)"_s;
    }
    return {};
}

QString YtdlpSource::quoteArgument(const QString &argument)
{
    // MSVCRT command-line rules: wrap in double quotes; every backslash run that precedes a
    // quote (including the closing one) is doubled, and embedded quotes are escaped. Doubling
    // unconditionally would corrupt Windows paths ending in a backslash.
    if (!argument.isEmpty() && !argument.contains(QLatin1Char(' '))
        && !argument.contains(QLatin1Char('\t')) && !argument.contains(QLatin1Char('"'))) {
        return argument;
    }

    QString quoted;
    quoted.reserve(argument.size() + 2);
    quoted += QLatin1Char('"');
    int backslashes = 0;
    for (const QChar &ch : argument) {
        if (ch == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            quoted.append(QString(backslashes * 2, QLatin1Char('\\')));
            quoted += QLatin1Char('\\'); // escape the quote itself
        } else {
            quoted.append(QString(backslashes, QLatin1Char('\\')));
        }
        backslashes = 0;
        quoted += ch;
    }
    quoted.append(QString(backslashes * 2, QLatin1Char('\\'))); // before the closing quote
    quoted += QLatin1Char('"');
    return quoted;
}

QStringList YtdlpSource::baseArguments(bool jsonProbe, double startSec) const
{
    QStringList arguments;
    arguments << u"--ignore-config"_s      // never let a user-level yt-dlp config change our flags
              << u"--quiet"_s              // keep stderr to real errors only (the drain thread tails it)
              << u"--no-warnings"_s
              << u"--no-progress"_s
              << u"--no-playlist"_s;       // a playlist URL must not turn into a multi-video feed

    if (jsonProbe) {
        // Machine-readable metadata for the duration/live probe. The user's extra args still
        // apply: cookies that satisfy a bot check help the probe exactly as they help the feed.
        arguments << u"-J"_s << u"--no-download"_s;
    } else {
        arguments << u"--concurrent-fragments"_s
                  << QString::number(qBound(1, m_config.concurrentFragments, 16))
                  // yt-dlp writes concurrently fetched fragments to stdout in index order,
                  // so the pipe stays one ordered stream while the fetch runs ahead.
                  << u"--retries"_s << u"5"_s   // transient network errors are common on long downloads
                  << u"--fragment-retries"_s << u"5"_s
                  << u"--socket-timeout"_s << u"10"_s;      // a dead connection must not stall the pipe forever
    }

    // When a merge does happen, yt-dlp must find our pinned ffmpeg.exe. A directory is not
    // enough on this layout (the exe sits in bin-video/, not next to the DLLs), so pass the
    // full path when it exists.
    // User extras go after our own options but before the URL, so a stray positional token can
    // never be mistaken for a second video.
    for (const QString &token : QProcess::splitCommand(m_config.extraArgs)) {
        arguments.append(token);
    }
    // Apply the saved selector to the metadata probe as well as playback. A probe
    // using yt-dlp's unrelated default could pick a different codec or resolution.
    const QString selector = m_config.formatSelector.trimmed();
    arguments << u"-f"_s << (selector.isEmpty() ? YouTubeSourceConfig {}.formatSelector : selector);
    const QString ffmpegExe = resolveFfmpegProgram();
    if (!ffmpegExe.isEmpty()) arguments << u"--ffmpeg-location"_s << ffmpegExe;
    if (!jsonProbe) {
        const bool transcode = youtubeNeedsVideoConversion(m_selectedVideoCodec, m_selectedDynamicRange,
                                                           m_decodingSinksOnly);
        const bool hdr = !m_selectedDynamicRange.isEmpty() && m_selectedDynamicRange != u"SDR"_s;
        QString outputOptions = youtubeFfmpegOutputOptions(
            transcode, hdr, m_useNvenc, m_config.audioBitrateKbps, m_separateInputs,
            !transcode && youtubeNativeVideo(m_selectedVideoCodec));
        // YouTube media URLs can stall indefinitely on HTTP range seeks. Skip
        // to the target on output: decode/discard converted frames, drop copied
        // packets up to the next keyframe, and keep the audio/video timestamps aligned.
        if (startSec > 0.0) outputOptions += u" -ss %1"_s.arg(startSec, 0, 'f', 3);
        arguments << u"--downloader"_s << u"ffmpeg"_s
                  << u"--downloader-args"_s << u"ffmpeg_i:-rw_timeout 10000000"_s
                  << u"--downloader-args"_s << (u"ffmpeg_o:"_s + outputOptions);
    }
    return arguments;
}

void YtdlpSource::probeMetadata(const QString &program)
{
    // Resolve the saved selection before deciding whether video needs conversion.
    // A failed probe must fail playback rather than pass an unknown codec to the sinks.
    QStringList arguments = baseArguments(true);
    arguments << u"--"_s << m_config.url.toString().trimmed();
    m_formatResolved = false;
    m_probeError = u"The selected video/audio formats could not be resolved. Refresh the format list and try again."_s;

    // Every exit below is "no direct URL": clear first so a failed re-probe in the
    // fallback ladder can never leave a stale URL that keeps directEligible() true and
    // loops the ladder forever.
    m_directUrl.clear();
    m_directHeaders.clear();

    QProcess probe;
    probe.setProgram(program);
    probe.setArguments(arguments);
    probe.start();
    if (!probe.waitForStarted(5000)) {
        m_probeError = probe.errorString();
        m_probed.store(true, std::memory_order_relaxed);
        return;
    }

    // Bounded wait that stays responsive to stop(): the worker thread has no event loop, so
    // the blocking waitForFinished is polled in slices. stdout is drained on every slice —
    // a -J payload is at most a few hundred KB, and anything larger (or anything that does
    // not start with a JSON brace) means this is not the metadata we asked for, so the probe
    // is abandoned rather than buffered. stderr is drained too so yt-dlp can never block.
    constexpr int kProbeBudgetMs = 20000;
    constexpr int kProbeSliceMs = 200;
    constexpr qint64 kProbeMaxBytes = 8 * 1024 * 1024;
    QByteArray stdoutBuffer;
    QByteArray stderrBuffer;
    bool jsonConfirmed = false;
    int waited = 0;
    bool finished = false;
    while (true) {
        stdoutBuffer += probe.readAllStandardOutput();
        stderrBuffer = (stderrBuffer + probe.readAllStandardError()).right(4096);

        if (!jsonConfirmed) {
            // The first non-whitespace byte decides: a real -J run answers with a JSON
            // object/array. Anything else (an error page, a misbehaving binary streaming
            // media bytes) ends the probe immediately.
            int first = -1;
            for (int i = 0; i < stdoutBuffer.size(); ++i) {
                const char c = stdoutBuffer.at(i);
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                    first = i;
                    break;
                }
            }
            if (first >= 0) {
                const char c = stdoutBuffer.at(first);
                if (c != '{' && c != '[') {
                    probe.kill();
                    probe.waitForFinished(1000);
                    m_probed.store(true, std::memory_order_relaxed);
                    return;
                }
                jsonConfirmed = true;
            }
        }

        if (stdoutBuffer.size() > kProbeMaxBytes) {
            probe.kill();
            probe.waitForFinished(1000);
            m_probed.store(true, std::memory_order_relaxed);
            return;
        }

        finished = probe.waitForFinished(kProbeSliceMs);
        if (finished) {
            stdoutBuffer += probe.readAllStandardOutput();
            break;
        }
        if (m_stop.load(std::memory_order_relaxed) || waited >= kProbeBudgetMs) {
            m_probeError = u"yt-dlp metadata query timed out or was cancelled"_s;
            probe.kill();
            probe.waitForFinished(1000);
            m_probed.store(true, std::memory_order_relaxed);
            return;
        }
        waited += kProbeSliceMs;
    }
    if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
        m_probeError += u"\n"_s + QString::fromUtf8(stderrBuffer + probe.readAllStandardError()).trimmed();
        m_probed.store(true, std::memory_order_relaxed);
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(stdoutBuffer);
    QJsonObject info = doc.isObject() ? doc.object() : QJsonObject {};
    if (doc.isArray() && !doc.array().isEmpty()) {
        info = doc.array().first().toObject(); // defensive: a playlist despite --no-playlist
    }

    const double duration = info.value(u"duration"_s).toDouble(0.0);
    bool live = info.value(u"is_live"_s).toBool(false);
    const QString liveStatus = info.value(u"live_status"_s).toString();
    if (liveStatus == u"is_live"_s || liveStatus == u"is_upcoming"_s || liveStatus == u"post_live"_s) {
        live = true;
    }

    m_durationSec.store(duration > 0.0 ? duration : 0.0, std::memory_order_relaxed);
    m_live.store(live, std::memory_order_relaxed);

    QJsonObject selectedVideo = info;
    const auto selectedFormats = info.value(u"requested_formats"_s).toArray();
    m_separateInputs = selectedFormats.size() > 1;
    for (const auto &value : selectedFormats) {
        const auto f = value.toObject();
        const auto codec = f.value(u"vcodec"_s).toString();
        if (!codec.isEmpty() && codec != u"none"_s) { selectedVideo = f; break; }
    }
    m_selectedVideoCodec = selectedVideo.value(u"vcodec"_s).toString();
    m_selectedDynamicRange = selectedVideo.value(u"dynamic_range"_s).toString();
    m_formatResolved = youtubeVideoPassthrough(m_selectedVideoCodec)
        || m_selectedVideoCodec.startsWith(u"vp9"_s) || m_selectedVideoCodec.startsWith(u"vp09"_s)
        || m_selectedVideoCodec.startsWith(u"av01"_s);

    // Only the actually selected bundled HLS stream may use the direct reader.
    // Separate inputs and HDR/codec conversion must go through the FFmpeg merge pipe.
    const auto audio = selectedVideo.value(u"acodec"_s).toString();
    if (m_config.directUrlMode != u"off"_s && !m_separateInputs
        && !youtubeNeedsVideoConversion(m_selectedVideoCodec, m_selectedDynamicRange)
        && selectedVideo.value(u"protocol"_s).toString().startsWith(u"m3u8"_s)
        && (audio.startsWith(u"mp4a"_s) || audio.startsWith(u"aac"_s))) {
        m_directUrl = selectedVideo.value(u"url"_s).toString();
        m_directHeaders = buildHeaderString(selectedVideo.value(u"http_headers"_s).toObject());
    }

    m_probed.store(true, std::memory_order_relaxed);

    qInfo("ytdlp: probed %s — duration %.1f s, %s%s", qUtf8Printable(m_config.url.toString()),
          m_durationSec.load(std::memory_order_relaxed),
          m_live.load(std::memory_order_relaxed) ? "live" : "VOD",
          m_directUrl.isEmpty() ? "" : ", direct URL available");
}

bool YtdlpSource::run()
{
    m_videoCodec.store(VideoCodec::Unknown, std::memory_order_relaxed);
    m_audioCodec.store(AudioCodec::Unknown, std::memory_order_relaxed);
    m_audioSampleRate.store(48000, std::memory_order_relaxed);
    m_audioChannels.store(2, std::memory_order_relaxed);

    QString error = m_pathError;
    const QString program = m_ytdlpProgram;
    if (program.isEmpty()) {
        Q_EMIT errorOccurred(error);
        setState(StreamState::Failed);
        return false;
    }

    setState(StreamState::Connecting);

    // The metadata probe runs once per source: the video identity does not change across
    // reconnects, and re-probing on every replay would add seconds to each restart.
    if (!m_probed.load(std::memory_order_relaxed)) {
        probeMetadata(program);
    }
    if (m_stop.load(std::memory_order_relaxed)) return true;
    if (!m_formatResolved) {
        m_probed.store(false, std::memory_order_relaxed); // Retry resolution after a transient probe failure.
        Q_EMIT errorOccurred(m_probeError);
        setState(StreamState::Failed);
        return false;
    }
    m_ffmpegProgram = resolveFfmpegProgram();
    if (m_ffmpegProgram.isEmpty() && !directEligible()) {
        Q_EMIT errorOccurred(u"FFmpeg is required to merge/convert the selected video and audio. Install ffmpeg or place ffmpeg.exe next to C-Bridge.exe."_s);
        setState(StreamState::Failed);
        return false;
    }
    if (youtubeNeedsVideoConversion(m_selectedVideoCodec, m_selectedDynamicRange, m_decodingSinksOnly)) {
        m_useNvenc = usableNvenc(m_ffmpegProgram, m_stop);
        setLastEvent(u"FFmpeg merge · %1→H.264 (%2)"_s.arg(m_selectedVideoCodec,
            m_useNvenc ? u"NVIDIA"_s : u"software"_s));
    } else if (youtubeNativeVideo(m_selectedVideoCodec)) {
        setLastEvent(u"FFmpeg merge · %1 passthrough to NDI + Opus"_s.arg(m_selectedVideoCodec));
    } else {
        setLastEvent(m_separateInputs ? u"FFmpeg merge · video passthrough + Opus"_s
                                     : u"FFmpeg pipe · video passthrough + Opus"_s);
    }

    // Resume position (plan §2): when the pipeline's reconnect timer restarted us after a
    // mid-stream failure, start at the playhead instead of at zero. A clean EOF never sets
    // this, so the VOD replay loop still begins from the top; a user stop() cleared it.
    double startSec = 0.0;
    const double resumeSec = m_pendingResumeSec.exchange(-1.0, std::memory_order_relaxed);
    if (resumeSec >= kResumeFloorSeconds && !m_live.load(std::memory_order_relaxed)) {
        startSec = resumeSec;
        // The sinks must wait for this section's first IDR, exactly as after a seek.
        m_freshGeneration.store(true, std::memory_order_relaxed);
        setLastEvent(u"resumed at %1 s after stall"_s.arg(resumeSec, 0, 'f', 1));
    }

    // Generations: a seek starts a new generation at the requested position; a mid-stream
    // failure retries in place at the playhead (up to kMaxStallRetries without a healthy
    // window); anything else ends the run and the pipeline's reconnect timer restarts us.
    // While the probe resolved a muxed-HLS URL, generations fetch it directly with
    // FFmpeg's HLS reader; a pre-delivery direct failure falls back to the yt-dlp pipe.
    for (;;) {
        GenerationResult result = GenerationResult::Failed;
        QString generationError;
        m_lastGenerationDelivered = false;

        if (directEligible()) {
            ++m_directAttempts;
            spawnDirect(startSec, &result, &generationError);
            if (m_lastGenerationDelivered) {
                // The direct generation carried media: its result stands, no pipe needed.
                // A generation that ran clean for kHealthyWindowSeconds earns a fresh
                // direct-attempt budget, so a long-lived stream keeps the fast path even
                // after a much later hiccup.
                const auto healthy = std::chrono::steady_clock::now() - m_deliveryStartTime;
                if (std::chrono::duration_cast<std::chrono::seconds>(healthy).count()
                    >= kHealthyWindowSeconds) {
                    m_directAttempts = 0;
                }
            } else {
                // The direct URL died before any media arrived. YouTube signs its URLs, so
                // a stale resolution is the most likely cause: re-probe once for a fresh
                // URL and retry the direct path; after that commit to the pipe for good.
                qInfo("ytdlp: direct URL attempt failed before delivery (%s), generation %d",
                      qUtf8Printable(generationError), m_directAttempts);
                if (m_directAttempts == 1) {
                    m_probed.store(false, std::memory_order_relaxed);
                    probeMetadata(program);
                    if (directEligible()) {
                        continue; // fresh URL — try the direct path again
                    }
                } else {
                    m_directGaveUp = true;
                    setLastEvent(u"pipe fallback (direct URL failed)"_s);
                }
                m_lastGenerationDelivered = false; // the pipe generation starts fresh
                spawnAndPump(program, startSec, &result, &generationError);
            }
        } else {
            spawnAndPump(program, startSec, &result, &generationError);
        }

        // An encoder may pass the capability check yet reject the actual dimensions
        // or run out of sessions. Retry once in software before any packet is delivered.
        if (result == GenerationResult::Failed && !m_lastGenerationDelivered && m_useNvenc
            && !m_stop.load(std::memory_order_relaxed)) {
            m_useNvenc = false;
            setLastEvent(u"FFmpeg merge · software fallback after NVIDIA output failure"_s);
            continue;
        }

        // A clean close well before the expected end is indistinguishable from a crashed
        // child or a truncated playlist: turn it into a failure so the resume logic below
        // applies. Only a genuine end of file (or of a live broadcast) ends the run
        // outright — and for VOD the pipeline's reconnect restarts the replay loop from
        // zero, which is the intended happy path.
        if (result == GenerationResult::Ended) {
            const double duration = m_durationSec.load(std::memory_order_relaxed);
            const double position = m_positionSec.load(std::memory_order_relaxed);
            const bool premature = m_lastGenerationDelivered
                && !m_live.load(std::memory_order_relaxed) && duration > 0.0
                && position < duration - 5.0;
            if (!premature) {
                Q_EMIT errorOccurred(describeFailure(u"The yt-dlp feed ended"_s));
                setState(StreamState::Failed);
                return false;
            }
            result = GenerationResult::Failed;
            generationError = u"the feed ended early at %1 s of %2 s"_s
                                  .arg(position, 0, 'f', 1)
                                  .arg(duration, 0, 'f', 1);
        }

        switch (result) {
        case GenerationResult::Stopped:
            return true;
        case GenerationResult::Seek:
            startSec = double(m_seekToMs.exchange(-1, std::memory_order_relaxed)) / 1000.0;
            continue;
        case GenerationResult::Ended:
            // Unreachable: handled above. Kept for exhaustiveness of the enum switch.
            Q_EMIT errorOccurred(describeFailure(u"The yt-dlp feed ended"_s));
            setState(StreamState::Failed);
            return false;
        case GenerationResult::Failed: {
            // In-source stall retry (plan §2): a generation that delivered media before
            // dying is a hiccup, not a broken feed. Respawn at the playhead without
            // bothering the pipeline — the sinks re-arm on the fresh generation's IDR and
            // the UI never sees a Failed state. A generation that ran clean for
            // kHealthyWindowSeconds earns a fresh retry budget.
            if (m_lastGenerationDelivered) {
                const auto healthy = std::chrono::steady_clock::now() - m_deliveryStartTime;
                if (std::chrono::duration_cast<std::chrono::seconds>(healthy).count()
                    >= kHealthyWindowSeconds) {
                    m_stallRetries = 0;
                }
                if (m_stallRetries < kMaxStallRetries) {
                    ++m_stallRetries;
                    if (m_live.load(std::memory_order_relaxed)) {
                        startSec = 0.0; // live has no timeline to resume — just re-join it
                    } else {
                        startSec = qMax(startSec, m_positionSec.load(std::memory_order_relaxed));
                        qInfo("ytdlp: resumed at %.1f s after stall (%d/%d)",
                              startSec, m_stallRetries, kMaxStallRetries);
                        setLastEvent(u"resumed at %1 s after stall (%2/%3)"_s
                                         .arg(startSec, 0, 'f', 1)
                                         .arg(m_stallRetries)
                                         .arg(kMaxStallRetries));
                    }
                    m_freshGeneration.store(true, std::memory_order_relaxed);
                    continue;
                }
                // Out of retries: hand the position to the pipeline's reconnect restart so
                // even the backoff path resumes at the playhead (plan §2).
                if (!m_live.load(std::memory_order_relaxed)) {
                    m_pendingResumeSec.store(qMax(startSec,
                                                  m_positionSec.load(std::memory_order_relaxed)),
                                             std::memory_order_relaxed);
                }
            }
            Q_EMIT errorOccurred(describeFailure(generationError));
            setState(StreamState::Failed);
            return false;
        }
        }
    }
}

bool YtdlpSource::spawnAndPump(const QString &program, double startSec,
                               GenerationResult *result, QString *error)
{
    // A converted seek may need to decode forward before the first output frame.
    // Once delivery starts, keep the usual short stall budget for network recovery.
    m_initialPipeStallSeconds = int(qBound(30.0, startSec, 180.0));
    // --- Job object ---------------------------------------------------------------
    // The PyInstaller onefile yt-dlp.exe is a bootloader parent plus the real Python child,
    // and it is the child that owns the stdout pipe handle. Killing only the top process
    // would leave the child writing into an orphaned pipe, so both go into a job object that
    // kills every member when its last handle closes (or explicitly on stop).
    m_job = CreateJobObjectW(nullptr, nullptr);
    if (!m_job) {
        *error = u"Could not create the yt-dlp job object: %1"_s.arg(long(GetLastError()));
        *result = GenerationResult::Failed;
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(static_cast<HANDLE>(m_job), JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
        const DWORD code = GetLastError();
        CloseHandle(m_job);
        m_job = nullptr;
        *error = u"Could not configure the yt-dlp job object: %1"_s.arg(long(code));
        *result = GenerationResult::Failed;
        return false;
    }

    // --- Pipes ----------------------------------------------------------------------
    SECURITY_ATTRIBUTES pipeAttributes {};
    pipeAttributes.nLength = sizeof(pipeAttributes);
    pipeAttributes.bInheritHandle = TRUE; // the child inherits the write ends

    HANDLE stdoutWrite = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&m_stdoutRead, &stdoutWrite, &pipeAttributes, 1024 * 1024) ||
        !CreatePipe(&m_stderrRead, &stderrWrite, &pipeAttributes, 64 * 1024)) {
        const DWORD code = GetLastError();
        if (m_stdoutRead) {
            CloseHandle(m_stdoutRead);
            m_stdoutRead = nullptr;
        }
        if (m_stderrRead) {
            CloseHandle(m_stderrRead);
            m_stderrRead = nullptr;
        }
        CloseHandle(m_job);
        m_job = nullptr;
        *error = u"Could not create the media pipe: %1"_s.arg(long(code));
        *result = GenerationResult::Failed;
        return false;
    }

    // Single teardown for every exit path below. Order: stop reading, kill the child (which
    // also unblocks the stderr drain thread at EOF), join it, close everything.
    AVFormatContext *format = nullptr;
    std::uint8_t *ioBuffer = static_cast<std::uint8_t *>(av_malloc(32768));
    AVIOContext *avio = nullptr;

    auto finish = [this, &format, &ioBuffer, &avio]() {
        if (format) {
            avformat_close_input(&format); // frees the context; CUSTOM_IO leaves pb to us
        }
        // FFmpeg never closes a caller-supplied AVIOContext (AVFMT_FLAG_CUSTOM_IO is set
        // automatically because we supplied pb — verified against this tree's demux.c), so
        // the context itself is ours to free. The buffer is too, but only via the context's
        // *current* pointer: avio_alloc_context takes ownership of ioBuffer, and the mpegts
        // demuxer's ffio_ensure_seekback() (mpegts.c, seekback = max(probesize, resync +
        // PROBE_PACKET_MAX_BUF) > our 32 KiB) av_free()s and replaces s->buffer on the first
        // read of an unseekable pipe. Freeing the original ioBuffer here would therefore be a
        // double free — it corrupts the heap and crashes in the next av_free, which is
        // m_transcoder.close()'s avcodec_free_context on stop. avio_context_free() does not
        // touch s->buffer (aviobuf.c), so release it ourselves. When avio was never created
        // (allocation failed) the buffer never left our hands and ioBuffer is still live.
        if (avio) {
            av_freep(&avio->buffer);
        } else {
            av_freep(&ioBuffer);
        }
        avio_context_free(&avio);
        killChild(); // no-op without a process
        if (m_stderrThread.joinable()) {
            m_stderrThread.join();
        }
        if (m_stdoutRead) {
            CloseHandle(m_stdoutRead);
            m_stdoutRead = nullptr;
        }
        if (m_stderrRead) {
            CloseHandle(m_stderrRead);
            m_stderrRead = nullptr;
        }
        if (m_process) {
            CloseHandle(m_process);
            m_process = nullptr;
        }
        if (m_job) {
            CloseHandle(m_job); // KILL_ON_JOB_CLOSE as a backstop for any straggler
            m_job = nullptr;
        }
        m_transcoder.close();
    };

    auto fail = [&finish, error, result](const QString &cause) {
        finish();
        *error = cause;
        *result = GenerationResult::Failed;
        return false;
    };

    // --- Command line -----------------------------------------------------------------
    QStringList arguments = baseArguments(false, startSec);
    arguments << u"-o"_s << u"-"_s << u"--"_s << m_config.url.toString().trimmed();

    QString commandLine = quoteArgument(program);
    for (const QString &argument : arguments) {
        commandLine += QLatin1Char(' ') + quoteArgument(argument);
    }

    STARTUPINFOW startupInfo {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    // A GUI app has no console, so STD_INPUT_HANDLE can be invalid. The child must still get a
    // valid handle (STARTF_USESTDHANDLES requires all three), and NUL keeps it from ever blocking
    // on an accidental prompt.
    HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    bool ownStdin = false;
    if (!stdinHandle || stdinHandle == INVALID_HANDLE_VALUE) {
        stdinHandle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        ownStdin = true;
    }
    startupInfo.hStdInput = stdinHandle;
    startupInfo.hStdOutput = stdoutWrite;
    startupInfo.hStdError = stderrWrite;

    PROCESS_INFORMATION processInfo {};
    // CreateProcessW may modify lpCommandLine, so it needs a mutable copy; the program name is
    // read-only. CREATE_NO_WINDOW keeps the console-subsystem yt-dlp.exe from flashing a window
    // (its output goes to our pipes anyway).
    const std::wstring programPath = program.toStdWString();
    std::wstring commandLineWide = commandLine.toStdWString();
    const BOOL spawned = CreateProcessW(programPath.c_str(), commandLineWide.data(), nullptr, nullptr,
                                        TRUE /* inherit the pipe write ends */, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startupInfo, &processInfo);
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);
    if (ownStdin) {
        CloseHandle(stdinHandle);
    }

    if (!spawned) {
        const DWORD code = GetLastError();
        return fail(u"Could not start yt-dlp (%1): %2"_s.arg(QFileInfo(program).fileName()).arg(long(code)));
    }

    m_process = processInfo.hProcess;
    CloseHandle(processInfo.hThread);

    // The child (and the PyInstaller bootloader) must die with us, no matter how stop() is
    // reached. Assigning after spawn keeps a failed CreateProcess from touching the job.
    if (!AssignProcessToJobObject(static_cast<HANDLE>(m_job), static_cast<HANDLE>(m_process))) {
        qWarning("ytdlp: could not assign yt-dlp to its job object (error %lu); "
                 "stop may leave orphan processes",
                 unsigned(GetLastError()));
    } else {
        m_assignedToJob = true;
    }

    m_lastByteTime = std::chrono::steady_clock::now();
    m_stderrThread = std::thread([this] { drainStderr(); });

    // --- Demux the pipe -----------------------------------------------------------------
    format = avformat_alloc_context();
    if (format && ioBuffer) {
        // Read-only: write/seek callbacks stay null. FFmpeg sets AVFMT_FLAG_CUSTOM_IO because we
        // supplied pb, so it will never close or free this context — finish() owns both the
        // buffer and the AVIOContext itself on every exit path (verified against demux.c).
        avio = avio_alloc_context(ioBuffer, 32768, 0, this, &pipeReadCallback, nullptr, nullptr);
    }
    if (!format || !avio) {
        return fail(u"Could not allocate the media demuxer"_s);
    }
    format->pb = avio;

    const int openRet = avformat_open_input(&format, "", nullptr, nullptr);
    if (openRet < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] {};
        av_strerror(openRet, errbuf, sizeof(errbuf));
        return fail(u"could not probe the yt-dlp output: %1"_s.arg(QString::fromUtf8(errbuf)));
    }

    // The pipe contract is MPEG-TS, or Matroska for VP9/AV1 passthrough. If yt-dlp produced
    // anything else (a selector that picked a progressive file, a failed merge, ...) the
    // demuxer would still "work" but the output would be unusable downstream — fail fast
    // with an actionable message instead.
    const bool nativeVideo =
        !youtubeNeedsVideoConversion(m_selectedVideoCodec, m_selectedDynamicRange, m_decodingSinksOnly)
        && youtubeNativeVideo(m_selectedVideoCodec);
    const QString expectedFormat = nativeVideo ? u"matroska,webm"_s : u"mpegts"_s;
    if (!format->iformat || QString::fromUtf8(format->iformat->name) != expectedFormat) {
        const QString got = format->iformat ? QString::fromUtf8(format->iformat->name) : u"(unknown)"_s;
        return fail(u"yt-dlp produced %1, expected %2 — check the format selector"_s.arg(got, expectedFormat));
    }

    // --- Identify the streams -------------------------------------------------------------
    int videoIndex = -1;
    int audioIndex = -1;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVCodecParameters *par = format->streams[i]->codecpar;
        const bool usableVideo = par->codec_id == AV_CODEC_ID_H264 || par->codec_id == AV_CODEC_ID_HEVC
            || (nativeVideo && (par->codec_id == AV_CODEC_ID_VP9 || par->codec_id == AV_CODEC_ID_AV1));
        if (videoIndex < 0 && par->codec_type == AVMEDIA_TYPE_VIDEO && usableVideo) {
            videoIndex = int(i);
        } else if (audioIndex < 0 && par->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioIndex = int(i);
        }
    }

    if (videoIndex < 0) {
        return fail(u"no usable video stream in the feed — check the format selector"_s);
    }

    VideoCodec videoCodec = VideoCodec::H264;
    switch (format->streams[videoIndex]->codecpar->codec_id) {
    case AV_CODEC_ID_HEVC: videoCodec = VideoCodec::H265; break;
    case AV_CODEC_ID_VP9: videoCodec = VideoCodec::Vp9; break;
    case AV_CODEC_ID_AV1: videoCodec = VideoCodec::Av1; break;
    default: break;
    }
    m_videoCodec.store(videoCodec, std::memory_order_relaxed);
    Q_EMIT videoCodecNegotiated(videoCodec);

    bool transcodingAudio = false;
    if (audioIndex >= 0) {
        const AVCodecParameters *par = format->streams[audioIndex]->codecpar;
        if (par->codec_id == AV_CODEC_ID_AAC || par->codec_id == AV_CODEC_ID_AAC_LATM) {
            // YouTube's AAC becomes Opus 48 kHz stereo before it reaches the pipeline, so every
            // sink sees exactly what WHEP and SRT deliver.
            const int rate = par->sample_rate > 0 ? par->sample_rate : 48000;
            const int channels = par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : 2;
            const int bitrateKbps = m_config.audioBitrateKbps > 0 ? m_config.audioBitrateKbps : 128;
            QString transcodeError;
            if (!m_transcoder.open(par->codec_id, rate, channels, bitrateKbps, &transcodeError)) {
                return fail(u"could not open the %1→Opus transcoder: %2"_s
                                .arg(QString::fromUtf8(avcodec_get_name(par->codec_id)))
                                .arg(transcodeError));
            }
            transcodingAudio = true;
            m_audioCodec.store(AudioCodec::Opus, std::memory_order_relaxed);
            m_audioSampleRate.store(48000, std::memory_order_relaxed);
            m_audioChannels.store(2, std::memory_order_relaxed);
        } else if (par->codec_id == AV_CODEC_ID_OPUS) {
            // Reserved branch: a pipe-safe H.264+Opus source would pass the audio through untouched.
            const int rate = par->sample_rate > 0 ? par->sample_rate : 48000;
            const int channels = par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : 2;
            m_audioCodec.store(AudioCodec::Opus, std::memory_order_relaxed);
            m_audioSampleRate.store(rate, std::memory_order_relaxed);
            m_audioChannels.store(channels, std::memory_order_relaxed);
        } else {
            qInfo("ytdlp: ignoring unsupported audio codec %d", int(par->codec_id));
        }
    }

    setState(StreamState::Running);

    // A seeked generation looks to the pipeline like a reconnect: tell it to re-arm the
    // keyframe gate so the sinks wait for this section's first IDR before reopening.
    if (startSec > 0.0) {
        m_freshGeneration.store(true, std::memory_order_relaxed);
    }
    m_positionSec.store(startSec, std::memory_order_relaxed);
    m_directActive = false;

    // --- Pump packets ---------------------------------------------------------------------
    pumpPackets(format, videoIndex, audioIndex, transcodingAudio, startSec,
                /*nativeSeek=*/false, result, error);

    // --- Teardown ---------------------------------------------------------------------------
    finish();

    if (*result == GenerationResult::Stopped || *result == GenerationResult::Seek) {
        m_paused.store(false, std::memory_order_relaxed); // a seek resumes from the new position
    }
    return *result == GenerationResult::Stopped;
}

void YtdlpSource::pumpPackets(AVFormatContext *format, int videoIndex, int audioIndex,
                              bool transcodingAudio, double startSec, bool nativeSeek,
                              GenerationResult *result, QString *error)
{
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        *error = u"Could not allocate the media packet"_s;
        *result = GenerationResult::Failed;
        return;
    }

    const AVRational videoClock { 1, 90000 };
    quint32 lastVideoTs = 0;
    bool haveLastVideoTs = false;
    // Audio RTP clock: anchor (first packet's PTS on the fixed 48 kHz clock) + 960 per Opus
    // frame. Both clocks start at stream open, so the TS muxer's PCR/PTS relationship stays
    // coherent for as long as the feed runs.
    quint32 audioAnchor = 0;
    bool haveAudioAnchor = false;
    std::uint64_t audioFrameIndex = 0;

    // Media clock for the UI: the generation's first video PTS defines the zero of the
    // child-local timeline (section downloads restart both clocks near zero, plain runs start
    // wherever the stream began), and every later packet advances the playhead from there.
    // An in-place seek (direct path) re-anchors both: the base becomes the seek target and
    // the first PTS is taken from the packet stream that follows the seek.
    std::int64_t firstVideoPts64 = AV_NOPTS_VALUE;
    double positionBaseSec = startSec;
    PlaybackClock playbackClock;

    // Matroska may keep AV1's sequence header only in av1C, but the decoders downstream get
    // no extradata, so keyframes carry it in-band like H.264 parameter sets.
    std::vector<std::uint8_t> av1ConfigObus;
    std::vector<std::uint8_t> av1Keyframe;
    const AVCodecParameters *videoPar = format->streams[videoIndex]->codecpar;
    if (videoPar->codec_id == AV_CODEC_ID_AV1 && videoPar->extradata_size > 4
        && (videoPar->extradata[0] & 0x80)) {
        av1ConfigObus.assign(videoPar->extradata + 4, videoPar->extradata + videoPar->extradata_size);
    }

    for (;;) {
        if (m_stop.load(std::memory_order_relaxed)) {
            *result = GenerationResult::Stopped;
            break;
        }

        if (m_seekToMs.load(std::memory_order_relaxed) >= 0) {
            if (!nativeSeek) {
                // The read loop cannot seek an unseekable pipe; tearing this child down and
                // letting run() respawn it at the new position is the honest model (plan §4.5).
                *result = GenerationResult::Seek;
                break;
            }
            // Direct path: serve the seek in place, the way mpv does — the HLS reader maps
            // the target to a segment index and refetches. Clear the request *before*
            // seeking so the interrupt callback stops firing for it (an interrupted
            // av_seek_frame would fail the very seek the user asked for), then re-anchor
            // every clock like a fresh generation and let the pipeline re-arm its gate.
            const double targetSec = double(m_seekToMs.exchange(-1, std::memory_order_relaxed)) / 1000.0;
            const std::int64_t target = av_rescale_q(qint64(targetSec * 1000.0),
                                                     AVRational { 1, 1000 },
                                                     format->streams[videoIndex]->time_base);
            if (av_seek_frame(format, videoIndex, target, AVSEEK_FLAG_BACKWARD) < 0) {
                *error = u"seek to %1 s failed"_s.arg(targetSec, 0, 'f', 1);
                *result = GenerationResult::Failed;
                break;
            }
            firstVideoPts64 = AV_NOPTS_VALUE;
            playbackClock.reset();
            haveLastVideoTs = false;
            haveAudioAnchor = false;
            audioFrameIndex = 0;
            positionBaseSec = targetSec;
            m_positionSec.store(targetSec, std::memory_order_relaxed);
            m_freshGeneration.store(true, std::memory_order_relaxed);
            setLastEvent(u"seeked to %1 s"_s.arg(targetSec, 0, 'f', 1));
            continue;
        }

        if (m_paused.load(std::memory_order_relaxed)) {
            // Stop draining: on the pipe path the ~1 MiB buffer fills and yt-dlp blocks on
            // OS backpressure; on the direct path the HLS reader simply stops fetching the
            // next segment. Either way: no bytes lost, no process churn. Stop and seek are
            // still polled above, so both work while paused.
            const auto pauseStart = PlaybackClock::Clock::now();
            while (m_paused.load(std::memory_order_relaxed)
                   && !m_stop.load(std::memory_order_relaxed)
                   && m_seekToMs.load(std::memory_order_relaxed) < 0) {
                Sleep(kPumpPollMs);
            }
            playbackClock.delay(PlaybackClock::Clock::now() - pauseStart);
            continue;
        }

        m_pumpActive.store(true, std::memory_order_relaxed); // direct path: allow seek interrupts
        const int ret = av_read_frame(format, packet);
        m_pumpActive.store(false, std::memory_order_relaxed);
        if (ret < 0) {
            if (m_stop.load(std::memory_order_relaxed)) {
                *result = GenerationResult::Stopped;
            } else if (m_seekToMs.load(std::memory_order_relaxed) >= 0) {
                // The interrupt callback aborted this read for a seek the loop handles at
                // the top of the next iteration.
                continue;
            } else if (ret == AVERROR_EOF) {
                // The feed closed cleanly: yt-dlp finished the video or the HLS playlist
                // ended. For a live feed this means the source ended; for VOD it is the
                // normal end of the file. Either way the pipeline's reconnect logic
                // restarts with backoff.
                *result = GenerationResult::Ended;
            } else if (ret == AVERROR(ETIMEDOUT)) {
                *error = nativeSeek
                    ? u"the HLS reader stalled (no data for %1 s)"_s.arg(kPipeStallSeconds)
                    : u"The yt-dlp pipe stalled (no data for %1 s)"_s.arg(kPipeStallSeconds);
                *result = GenerationResult::Failed;
            } else {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] {};
                av_strerror(ret, errbuf, sizeof(errbuf));
                *error = u"read failed: %1"_s.arg(QString::fromUtf8(errbuf));
                *result = GenerationResult::Failed;
            }
            break;
        }

        if ((packet->stream_index == videoIndex || packet->stream_index == audioIndex)
            && packet->size > 0) {
            const auto timestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
            if (timestamp != AV_NOPTS_VALUE) {
                const auto mediaUs = av_rescale_q(timestamp,
                    format->streams[packet->stream_index]->time_base, AVRational { 1, 1000000 });
                auto due = playbackClock.deadline(mediaUs, PlaybackClock::Clock::now());
                while (!m_stop.load(std::memory_order_relaxed)
                       && m_seekToMs.load(std::memory_order_relaxed) < 0) {
                    if (m_paused.load(std::memory_order_relaxed)) {
                        const auto pauseStart = PlaybackClock::Clock::now();
                        while (m_paused.load(std::memory_order_relaxed)
                               && !m_stop.load(std::memory_order_relaxed)
                               && m_seekToMs.load(std::memory_order_relaxed) < 0) {
                            Sleep(kPumpPollMs);
                        }
                        const auto pausedFor = PlaybackClock::Clock::now() - pauseStart;
                        playbackClock.delay(pausedFor);
                        due += pausedFor;
                    }
                    const auto remaining = due - PlaybackClock::Clock::now();
                    if (remaining <= PlaybackClock::Clock::duration::zero()) {
                        break;
                    }
                    std::this_thread::sleep_for(std::min(remaining,
                        std::chrono::duration_cast<PlaybackClock::Clock::duration>(
                            std::chrono::milliseconds(kPumpPollMs))));
                }
                if (m_stop.load(std::memory_order_relaxed)
                    || m_seekToMs.load(std::memory_order_relaxed) >= 0) {
                    av_packet_unref(packet);
                    continue;
                }
            }
        }

        if (packet->stream_index == videoIndex && packet->size > 0) {
            // Two clocks on the 90 kHz grid: DTS in decode order (what muxers interleave and
            // keep monotonic) and PTS in presentation order (what players display). YouTube's
            // HLS avc1 carries B-frames, so they differ by the reordering depth — collapsing
            // them into one value is what made downstream playheads jump back and forth.
            std::int64_t dts64 = -1;
            std::int64_t pts64 = -1;
            if (packet->dts != AV_NOPTS_VALUE && packet->dts >= 0) {
                dts64 = av_rescale_q(packet->dts, format->streams[videoIndex]->time_base, videoClock);
            }
            if (packet->pts != AV_NOPTS_VALUE && packet->pts >= 0) {
                pts64 = av_rescale_q(packet->pts, format->streams[videoIndex]->time_base, videoClock);
            }

            quint32 dtsTs;
            quint32 ptsTs;
            if (dts64 < 0 && pts64 < 0) {
                // No presentation info at all: keep both clocks advancing at 30 fps.
                dtsTs = haveLastVideoTs ? lastVideoTs + 3000 : 0; // 3000 ticks @90 kHz = 1/30 s
                ptsTs = dtsTs;
            } else {
                if (dts64 < 0) {
                    dts64 = pts64; // DTS missing: decode order equals display order here
                }
                if (pts64 < 0) {
                    pts64 = dts64; // PTS missing: fall back to the decode clock
                }
                dtsTs = quint32(dts64);
                ptsTs = quint32(pts64);
            }
            lastVideoTs = dtsTs;
            haveLastVideoTs = true;

            // The UI playhead follows presentation time (what is on screen), anchored at this
            // generation's first PTS. In decode order the PTS wanders by the B-frame depth,
            // which is invisible at the 1 Hz stats poll and more accurate than DTS on average.
            if (pts64 >= 0) {
                if (firstVideoPts64 == AV_NOPTS_VALUE) {
                    firstVideoPts64 = pts64;
                }
                m_positionSec.store(positionBaseSec + double(pts64 - firstVideoPts64) / 90000.0,
                                    std::memory_order_relaxed);
            }

            // First video packet of the generation: this is what makes a later failure a
            // resumable stall rather than a never-started feed (plan §2).
            if (!m_lastGenerationDelivered) {
                m_lastGenerationDelivered = true;
                m_deliveryStartTime = std::chrono::steady_clock::now();
            }

            if (!av1ConfigObus.empty() && (packet->flags & AV_PKT_FLAG_KEY)) {
                av1Keyframe.assign(av1ConfigObus.begin(), av1ConfigObus.end());
                av1Keyframe.insert(av1Keyframe.end(), packet->data, packet->data + packet->size);
                m_onVideo(av1Keyframe.data(), av1Keyframe.size(), dtsTs, ptsTs);
            } else {
                m_onVideo(packet->data, std::size_t(packet->size), dtsTs, ptsTs);
            }
        } else if (audioIndex >= 0 && packet->stream_index == audioIndex && packet->size > 0) {
            const std::int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            if (!haveAudioAnchor) {
                audioAnchor = (pts >= 0)
                    ? quint32(av_rescale_q(pts, format->streams[audioIndex]->time_base, AVRational { 1, 48000 }))
                    : 0;
                haveAudioAnchor = true;
            }

            if (transcodingAudio) {
                QString transcodeError;
                const bool ok = m_transcoder.transcode(
                    packet->data, std::size_t(packet->size), pts,
                    [this, &audioAnchor, &audioFrameIndex](const std::uint8_t *opusData, std::size_t opusSize) {
                        // 960 samples per 20 ms Opus frame on the fixed 48 kHz clock.
                        m_onAudio(opusData, opusSize, audioAnchor + quint32(960u * (audioFrameIndex++)));
                    },
                    &transcodeError);
                if (!ok) {
                    *error = u"AAC→Opus transcode failed: %1"_s.arg(transcodeError);
                    *result = GenerationResult::Failed;
                    av_packet_unref(packet);
                    break;
                }
            } else {
                // Opus passthrough (reserved branch): rescale to the fixed 48 kHz clock.
                const std::int64_t samples = pts >= 0
                    ? av_rescale_q(pts, format->streams[audioIndex]->time_base, AVRational { 1, 48000 })
                    : -1;
                m_onAudio(packet->data, std::size_t(packet->size),
                          samples >= 0 ? quint32(samples) : audioAnchor + quint32(960u * (audioFrameIndex++)));
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    if (*result == GenerationResult::Stopped || *result == GenerationResult::Seek) {
        m_paused.store(false, std::memory_order_relaxed); // a seek resumes from the new position
    }
}

bool YtdlpSource::openDirectFormat(AVFormatContext **outFormat, int *videoIndex, int *audioIndex,
                                   bool *transcodingAudio, QString *error)
{
    AVFormatContext *format = avformat_alloc_context();
    if (!format) {
        *error = u"Could not allocate the HLS demuxer"_s;
        return false;
    }
    // Same interruptible-read contract the pipe path implements by hand: a blocking
    // network read aborts when stop (or, mid-pump, a seek) was requested.
    format->interrupt_callback.callback = &YtdlpSource::directInterruptCallback;
    format->interrupt_callback.opaque = this;

    AVDictionary *options = nullptr;
    av_dict_set(&options, "headers", m_directHeaders.toUtf8().constData(), 0);
    av_dict_set(&options, "reconnect", "1", 0);            // retry a dropped segment connection
    av_dict_set(&options, "reconnect_streamed", "1", 0);   // ... even mid-segment, from its start
    av_dict_set(&options, "reconnect_delay_max", "5", 0);  // bounded backoff inside one read
    av_dict_set(&options, "http_persistent", "1", 0);      // keep-alive across segments
    av_dict_set(&options, "rw_timeout", "10000000", 0);    // 10 s per socket op, then interrupt
    av_dict_set(&options, "allowed_extensions", "ALL", 0); // YouTube's keys are .key, not .txt

    int ret = avformat_open_input(&format, m_directUrl.toUtf8().constData(), nullptr, &options);
    av_dict_free(&options);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        avformat_close_input(&format); // also frees the context when open failed on its probe
        *error = u"could not open the direct HLS URL: %1"_s.arg(QString::fromUtf8(errbuf));
        return false;
    }

    ret = avformat_find_stream_info(format, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] {};
        av_strerror(ret, errbuf, sizeof(errbuf));
        avformat_close_input(&format);
        *error = u"could not read the direct HLS streams: %1"_s.arg(QString::fromUtf8(errbuf));
        return false;
    }

    int vIndex = -1;
    int aIndex = -1;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVCodecParameters *par = format->streams[i]->codecpar;
        if (vIndex < 0 && par->codec_type == AVMEDIA_TYPE_VIDEO
            && (par->codec_id == AV_CODEC_ID_H264 || par->codec_id == AV_CODEC_ID_HEVC)) {
            vIndex = int(i);
        } else if (aIndex < 0 && par->codec_type == AVMEDIA_TYPE_AUDIO) {
            aIndex = int(i);
        }
    }
    if (vIndex < 0) {
        avformat_close_input(&format);
        *error = u"the direct HLS URL carries no H.264/HEVC video stream"_s;
        return false;
    }

    const VideoCodec videoCodec =
        format->streams[vIndex]->codecpar->codec_id == AV_CODEC_ID_HEVC ? VideoCodec::H265 : VideoCodec::H264;
    m_videoCodec.store(videoCodec, std::memory_order_relaxed);
    Q_EMIT videoCodecNegotiated(videoCodec);

    bool transcoding = false;
    if (aIndex >= 0) {
        const AVCodecParameters *par = format->streams[aIndex]->codecpar;
        if (par->codec_id == AV_CODEC_ID_AAC || par->codec_id == AV_CODEC_ID_AAC_LATM) {
            const int rate = par->sample_rate > 0 ? par->sample_rate : 48000;
            const int channels = par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : 2;
            const int bitrateKbps = m_config.audioBitrateKbps > 0 ? m_config.audioBitrateKbps : 128;
            QString transcodeError;
            // HLS fMP4 delivers raw AAC with the AudioSpecificConfig in the init segment,
            // which the demuxer hands over as extradata; the decoder needs it (the TS/ADTS
            // pipe path leaves it empty and self-syncs on the ADTS header instead).
            const QByteArray extradata(
                reinterpret_cast<const char *>(par->extradata),
                par->extradata ? std::size_t(par->extradata_size) : std::size_t(0));
            if (!m_transcoder.open(par->codec_id, rate, channels, bitrateKbps, &transcodeError,
                                   extradata)) {
                avformat_close_input(&format);
                *error = u"could not open the %1→Opus transcoder: %2"_s
                             .arg(QString::fromUtf8(avcodec_get_name(par->codec_id)))
                             .arg(transcodeError);
                return false;
            }
            transcoding = true;
            m_audioCodec.store(AudioCodec::Opus, std::memory_order_relaxed);
            m_audioSampleRate.store(48000, std::memory_order_relaxed);
            m_audioChannels.store(2, std::memory_order_relaxed);
        } else if (par->codec_id == AV_CODEC_ID_OPUS) {
            const int rate = par->sample_rate > 0 ? par->sample_rate : 48000;
            const int channels = par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : 2;
            m_audioCodec.store(AudioCodec::Opus, std::memory_order_relaxed);
            m_audioSampleRate.store(rate, std::memory_order_relaxed);
            m_audioChannels.store(channels, std::memory_order_relaxed);
        } else {
            qInfo("ytdlp: direct path ignoring unsupported audio codec %d", int(par->codec_id));
        }
    }

    *outFormat = format;
    *videoIndex = vIndex;
    *audioIndex = aIndex;
    *transcodingAudio = transcoding;
    return true;
}

void YtdlpSource::spawnDirect(double startSec, GenerationResult *result, QString *error)
{
    m_directActive = true;
    // The direct path has no child, so describeFailure() must not quote a previous pipe
    // generation's stderr tail.
    {
        std::lock_guard lock(m_stderrMutex);
        m_stderrTail.clear();
    }
    m_exitCodeValid = false;

    AVFormatContext *format = nullptr;
    int videoIndex = -1;
    int audioIndex = -1;
    bool transcodingAudio = false;
    if (!openDirectFormat(&format, &videoIndex, &audioIndex, &transcodingAudio, error)) {
        m_directActive = false;
        *result = GenerationResult::Failed;
        return;
    }

    auto teardown = [&format]() {
        avformat_close_input(&format);
    };

    if (startSec > 0.0) {
        // Resume/seek onto the direct URL: the HLS demuxer is seekable (it maps time to a
        // segment index), so no child respawn and no section extraction — just seek and read.
        const std::int64_t target = av_rescale_q(qint64(startSec * 1000.0), AVRational { 1, 1000 },
                                                 format->streams[videoIndex]->time_base);
        if (av_seek_frame(format, videoIndex, target, AVSEEK_FLAG_BACKWARD) < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] {};
            av_strerror(AVERROR(EIO), errbuf, sizeof(errbuf));
            teardown();
            m_transcoder.close();
            m_directActive = false;
            *error = u"could not seek the direct HLS URL to %1 s (%2)"_s
                         .arg(startSec, 0, 'f', 1)
                         .arg(QString::fromUtf8(errbuf));
            *result = GenerationResult::Failed;
            return;
        }
    }

    setState(StreamState::Running);
    if (startSec > 0.0) {
        m_freshGeneration.store(true, std::memory_order_relaxed);
    }
    m_positionSec.store(startSec, std::memory_order_relaxed);
    setLastEvent(u"direct-url HLS (no yt-dlp pipe)"_s);

    pumpPackets(format, videoIndex, audioIndex, transcodingAudio, startSec,
                /*nativeSeek=*/true, result, error);

    teardown();
    m_transcoder.close();
    m_directActive = false;
}

int YtdlpSource::directInterruptCallback(void *opaque)
{
    auto *self = static_cast<YtdlpSource *>(opaque);
    if (self->m_stop.load(std::memory_order_relaxed)) {
        return 1;
    }
    // A pending seek may only abort a mid-pump read, never the initial open — interrupting
    // the open would turn a user action into a feed error (see m_pumpActive).
    return self->m_pumpActive.load(std::memory_order_relaxed)
            && self->m_seekToMs.load(std::memory_order_relaxed) >= 0
        ? 1
        : 0;
}

int YtdlpSource::pipeReadCallback(void *opaque, std::uint8_t *buf, int bufSize)
{
    auto *self = static_cast<YtdlpSource *>(opaque);

    for (;;) {
        if (self->m_stop.load(std::memory_order_relaxed)) {
            // Unblocks av_read_frame/avformat_open_input promptly on stop(). ETIMEDOUT keeps
            // the demuxer from treating a user-initiated stop as a feed error.
            return AVERROR(ETIMEDOUT);
        }

        if (self->m_seekToMs.load(std::memory_order_relaxed) >= 0) {
            // A seek was requested while this read was blocked: unblock so the pump loop can
            // notice the request and tear the generation down.
            return AVERROR(ETIMEDOUT);
        }

        DWORD available = 0;
        if (!PeekNamedPipe(static_cast<HANDLE>(self->m_stdoutRead), nullptr, 0, nullptr, &available, nullptr)) {
            // The pipe is broken: every write end has closed (yt-dlp exited or crashed).
            const BOOL exited = self->m_process && WaitForSingleObject(static_cast<HANDLE>(self->m_process), 0) == WAIT_OBJECT_0;
            return exited ? AVERROR_EOF : AVERROR(EIO); // Returning 0 would make AVIO retry forever.
        }

        if (available > 0) {
            break;
        }

        // No bytes yet. A stalled pipe (no data for kPipeStallSeconds) is reported as a timeout
        // so the pipeline's reconnect logic can restart the source with backoff. While paused
        // the stall clock is stopped: the child is blocked on the full pipe by design, and an
        // empty pipe is the expected steady state, not a failure.
        if (!self->m_paused.load(std::memory_order_relaxed)) {
            const auto now = std::chrono::steady_clock::now();
            const int budget = self->m_lastGenerationDelivered ? kPipeStallSeconds : self->m_initialPipeStallSeconds;
            if (std::chrono::duration_cast<std::chrono::seconds>(now - self->m_lastByteTime).count() >= budget) {
                return AVERROR(ETIMEDOUT);
            }
        }
        Sleep(20); // short sleep keeps stop() responsive without burning the CPU
    }

    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(self->m_stdoutRead), buf, static_cast<DWORD>(bufSize), &got, nullptr)) {
        return AVERROR(EIO);
    }
    if (got == 0) {
        // ReadFile reports zero bytes once every write end has closed.
        return AVERROR_EOF;
    }
    self->m_lastByteTime = std::chrono::steady_clock::now();
    return static_cast<int>(got);
}

void YtdlpSource::killChild()
{
    if (!m_process) {
        return;
    }
    // Kill every member of the job: the PyInstaller bootloader and the real Python child. The
    // TerminateProcess backstop covers the (logged) case where the job assignment failed.
    if (m_job && m_assignedToJob) {
        TerminateJobObject(static_cast<HANDLE>(m_job), 1);
    } else {
        TerminateProcess(static_cast<HANDLE>(m_process), 1); // no-op if already exited
    }
    WaitForSingleObject(static_cast<HANDLE>(m_process), kKillWaitMs);

    DWORD code = 0;
    m_exitCodeValid = GetExitCodeProcess(static_cast<HANDLE>(m_process), &code) && code != STILL_ACTIVE;
    if (m_exitCodeValid) {
        m_exitCode = code;
    }
}

void YtdlpSource::drainStderr()
{
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(static_cast<HANDLE>(m_stderrRead), buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        std::lock_guard lock(m_stderrMutex);
        m_stderrTail.append(buffer, static_cast<int>(got));
        if (m_stderrTail.size() > 8192) {
            m_stderrTail = m_stderrTail.right(4096); // keep the tail for diagnostics
        }
    }
}

QString YtdlpSource::describeFailure(const QString &cause) const
{
    QString message = cause;

    if (m_exitCodeValid && m_exitCode != 0) {
        message += u" — yt-dlp exited with code %1"_s.arg(long(m_exitCode));
    }

    QString tail;
    {
        std::lock_guard lock(m_stderrMutex);
        tail = m_stderrTail.trimmed();
    }
    if (tail.size() > 500) {
        tail = u"…" + tail.right(500); // the last lines carry yt-dlp's own diagnosis
    }
    if (!tail.isEmpty()) {
        message += u": " + tail;
    }

    // Known failure modes get an actionable hint (see YTDLP_SOURCE_PLAN.md §4.4).
    if (tail.contains(u"Sign in to confirm", Qt::CaseInsensitive) || tail.contains(u"bot check", Qt::CaseInsensitive)) {
        message += u" — YouTube is asking for a sign-in; add --cookies-from-browser chrome to the extra args";
    } else if (tail.contains(u"no matching format", Qt::CaseInsensitive)
               || tail.contains(u"Requested format is not available", Qt::CaseInsensitive)) {
        message += u" — clear the format selector field and retry with YouTube's default selection";
    } else if (tail.contains(u"js runtime", Qt::CaseInsensitive) || tail.contains(u"deno", Qt::CaseInsensitive)) {
        message += u" — this yt-dlp build needs a JS runtime; add --js-runtimes deno:<path> to the extra args";
    } else if (tail.contains(u"ffmpeg", Qt::CaseInsensitive) && tail.contains(u"not found", Qt::CaseInsensitive)) {
        message += u" — yt-dlp needs ffmpeg.exe to merge formats; check the FFmpeg build tree or pick a single-format selector";
    }
    return message;
}

} // namespace CBridge
