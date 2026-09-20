/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/streampipeline.h"

#include "media/opuspayloadsanitizer.h"
#include "sinks/streamsink.h"
#include "sinks/rtpmulticastsink.h"
#include "sinks/rtspsink.h"
#include "sinks/tsmulticastsink.h"
#include "webrtc/webrtcsource.h"

#ifdef CBRIDGE_NDI_SUPPORT
#include "sinks/ndisink.h"
#endif

#include <QDateTime>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

StreamPipeline::StreamPipeline(StreamConfig config, QObject *parent)
    : QObject(parent)
    , m_config(std::move(config))
    , m_reconnectDelayMs(m_config.reconnectInitialMs)
{
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this] {
        if (m_running.load(std::memory_order_relaxed) && m_source) {
            m_source->start();
        }
    });
}

StreamPipeline::~StreamPipeline()
{
    stop();
}

void StreamPipeline::setPassword(const QString &password)
{
    m_password = password;
}

void StreamPipeline::setState(StreamState state)
{
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.state = state;
    }
    Q_EMIT stateChanged(m_config.id, state);
}

StreamStats StreamPipeline::stats() const
{
    std::lock_guard lock(m_statsMutex);
    StreamStats copy = m_stats;
    copy.queueDepth = int(m_queue.size());
    copy.droppedFrames = m_queue.dropped();
    return copy;
}

QList<SinkStats> StreamPipeline::sinkStats() const
{
    std::lock_guard lock(m_statsMutex);
    return m_sinkStats;
}

// Snapshots the sinks, which only their owning thread may touch, into a locked copy.
void StreamPipeline::updateSinkStats()
{
    QList<SinkStats> snapshot;
    snapshot.reserve(int(m_sinks.size()));
    for (const auto &sink : m_sinks) {
        SinkStats entry;
        entry.description = sink->describe();
        entry.open = sink->isOpen();
        entry.bytesWritten = sink->bytesWritten();
        snapshot.append(entry);
    }

    std::lock_guard lock(m_statsMutex);
    m_sinkStats = std::move(snapshot);
}

void StreamPipeline::start()
{
    if (m_running.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    m_sawKeyframe = false;
    m_sinksOpen = false;
    m_parameterSets.clear();
    m_parameterSetsConsumed = false;
    m_reconnectDelayMs = m_config.reconnectInitialMs;

    for (const SinkConfig &sinkConfig : m_config.sinks) {
        if (!sinkConfig.enabled) {
            continue;
        }
        switch (sinkConfig.kind) {
        case SinkKind::TsMulticast:
            m_sinks.push_back(std::make_unique<TsMulticastSink>(sinkConfig.ts));
            break;
        case SinkKind::RtpMulticast:
            m_sinks.push_back(std::make_unique<RtpMulticastSink>(sinkConfig.rtp));
            break;
        case SinkKind::RtspUnicast:
            m_sinks.push_back(std::make_unique<RtspSink>(sinkConfig.rtsp, m_config.name));
            break;
        case SinkKind::Ndi:
#ifdef CBRIDGE_NDI_SUPPORT
            m_sinks.push_back(std::make_unique<NdiSink>(sinkConfig.ndi, m_config.name));
#else
            qWarning("Stream %s: NDI sink skipped, this build has no NDI support",
                     qUtf8Printable(m_config.name));
#endif
            break;
        }
    }

    if (m_sinks.empty()) {
        Q_EMIT errorOccurred(m_config.id, u"No usable sink is configured"_s);
        setState(StreamState::Failed);
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

    updateSinkStats();

    m_worker = std::thread([this] { workerLoop(); });

    m_source = new WebRtcSource(this);
    m_source->setConfig(m_config);
    m_source->setPassword(m_password);

    m_source->setVideoCallback([this](const std::uint8_t *data, std::size_t size, quint32 ts) {
        MediaUnit unit;
        unit.kind = MediaUnit::Kind::Video;
        unit.rtpTimestamp = ts;
        unit.payload.assign(data, data + size);
        m_queue.push(std::move(unit));
    });

    if (m_config.audioEnabled) {
        m_source->setAudioCallback([this](const std::uint8_t *data, std::size_t size, quint32 ts) {
            MediaUnit unit;
            unit.kind = MediaUnit::Kind::Audio;
            unit.rtpTimestamp = ts;
            unit.payload.assign(data, data + size);
            m_queue.push(std::move(unit));
        });
    }

    connect(m_source, &WebRtcSource::videoCodecNegotiated, this, [this](VideoCodec codec) {
        m_inspector.init(codec);
        std::lock_guard lock(m_statsMutex);
        m_stats.videoCodec = codec;
    });

    connect(m_source, &WebRtcSource::stateChanged, this, [this](StreamState state) {
        setState(state);
        if (state == StreamState::Running) {
            m_reconnectDelayMs = m_config.reconnectInitialMs;
        } else if (state == StreamState::Failed) {
            scheduleReconnect();
        }
    });

    connect(m_source, &WebRtcSource::errorOccurred, this, [this](const QString &message) {
        {
            std::lock_guard lock(m_statsMutex);
            m_stats.lastError = message;
        }
        Q_EMIT errorOccurred(m_config.id, message);
    });

    m_source->start();
}

void StreamPipeline::scheduleReconnect()
{
    if (!m_running.load(std::memory_order_relaxed)) {
        return;
    }

    closeSinks();
    m_sawKeyframe = false;
    m_parameterSets.clear();
    m_parameterSetsConsumed = false;

    {
        std::lock_guard lock(m_statsMutex);
        ++m_stats.reconnectCount;
    }

    setState(StreamState::Retrying);
    m_reconnectTimer->start(m_reconnectDelayMs);
    m_reconnectDelayMs = std::min(m_reconnectDelayMs * 2, m_config.reconnectMaxMs);
}

void StreamPipeline::stop()
{
    if (!m_running.exchange(false, std::memory_order_relaxed)) {
        return;
    }

    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }

    if (m_source) {
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
    }

    m_queue.stop();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_queue.clear();

    closeSinks();
    m_sinks.clear();

    {
        std::lock_guard lock(m_statsMutex);
        m_sinkStats.clear();
    }

    setState(StreamState::Idle);
}

bool StreamPipeline::openSinks()
{
    StreamFormat format;
    format.videoCodec = m_source ? m_source->negotiatedVideoCodec() : VideoCodec::Unknown;
    format.hasAudio = m_config.audioEnabled;
    format.videoExtradata = QByteArray(reinterpret_cast<const char *>(m_parameterSets.data()),
                                       qsizetype(m_parameterSets.size()));

    {
        std::lock_guard lock(m_statsMutex);
        format.width = m_stats.width;
        format.height = m_stats.height;
    }

    bool anyOpen = false;
    for (auto &sink : m_sinks) {
        QString error;
        if (sink->open(format, &error)) {
            anyOpen = true;
            qInfo("Stream %s: sink %s open", qUtf8Printable(m_config.name),
                  qUtf8Printable(sink->describe()));
        } else {
            qWarning("Stream %s: sink %s failed to open: %s", qUtf8Printable(m_config.name),
                     qUtf8Printable(sink->describe()), qUtf8Printable(error));
            Q_EMIT errorOccurred(m_config.id, error);
        }
    }

    m_sinksOpen = anyOpen;
    updateSinkStats();
    return anyOpen;
}

void StreamPipeline::closeSinks()
{
    for (auto &sink : m_sinks) {
        sink->close();
    }
    m_sinksOpen = false;
    updateSinkStats();
}

void StreamPipeline::handleVideoUnit(const MediaUnit &unit)
{
    const BitstreamInspector::Result info =
        m_inspector.inspect(unit.payload.data(), unit.payload.size());

    if (info.hasParameterSets) {
        std::vector<std::uint8_t> sets = BitstreamInspector::extractParameterSets(
            m_inspector.codec(), unit.payload.data(), unit.payload.size());
        if (!sets.empty()) {
            if (m_parameterSetsConsumed) {
                m_parameterSets.clear();
                m_parameterSetsConsumed = false;
            }
            m_parameterSets.insert(m_parameterSets.end(), sets.begin(), sets.end());
        }
    }

    if (info.width > 0 && info.height > 0) {
        std::lock_guard lock(m_statsMutex);
        m_stats.width = info.width;
        m_stats.height = info.height;
    }

    // Sinks need the parameter sets, not a resolution: FFmpeg's parser only reports
    // width one access unit late, which would drop the very keyframe that carries them.
    if (!m_sawKeyframe) {
        if (!info.isKeyframe || m_parameterSets.empty()) {
            return;
        }
        m_sawKeyframe = true;
        if (!openSinks()) {
            return;
        }
    }

    const std::uint8_t *payload = unit.payload.data();
    std::size_t payloadSize = unit.payload.size();
    if (info.isKeyframe && !info.hasParameterSets && !m_parameterSets.empty()) {
        m_assembledUnit.clear();
        m_assembledUnit.reserve(m_parameterSets.size() + unit.payload.size());
        m_assembledUnit.insert(m_assembledUnit.end(),
                               m_parameterSets.begin(), m_parameterSets.end());
        m_assembledUnit.insert(m_assembledUnit.end(),
                               unit.payload.begin(), unit.payload.end());
        payload = m_assembledUnit.data();
        payloadSize = m_assembledUnit.size();
        m_parameterSetsConsumed = true;
    } else if (info.isKeyframe) {
        m_parameterSetsConsumed = true;
    }

    for (auto &sink : m_sinks) {
        if (sink->isOpen()) {
            sink->writeVideo(payload, payloadSize, unit.rtpTimestamp, info.isKeyframe);
        }
    }

    updateSinkStats();

    std::lock_guard lock(m_statsMutex);
    ++m_stats.videoFramesIn;
    m_stats.bytesIn += unit.payload.size();
    m_stats.lastFrameEpochMs = QDateTime::currentMSecsSinceEpoch();
}

void StreamPipeline::handleAudioUnit(const MediaUnit &unit)
{
    if (!m_sinksOpen) {
        return;
    }

    // Some upstream encoders append undeclared trailing bytes to their Opus payloads, which makes
    // CBR packets structurally invalid for every conforming decoder (the NDI path hides this in
    // AudioDecoder's size-1 retry). Recover the largest valid prefix once here so the passthrough
    // sinks (TS multicast, RTP multicast, RTSP) deliver playable audio.
    const std::size_t payloadSize = OpusPayloadSanitizer::validPrefixLength(unit.payload.data(), unit.payload.size());
    if (payloadSize != unit.payload.size() && !m_warnedOpusTrailingBytes.exchange(true)) {
        qWarning("stream %s: upstream Opus payloads carry undeclared trailing bytes "
                 "(first packet %zu -> %zu); stripping them before muxing",
                 qUtf8Printable(m_config.id), unit.payload.size(), payloadSize);
    }

    for (auto &sink : m_sinks) {
        if (sink->isOpen()) {
            sink->writeAudio(unit.payload.data(), payloadSize, unit.rtpTimestamp);
        }
    }

    std::lock_guard lock(m_statsMutex);
    ++m_stats.audioFramesIn;
    m_stats.bytesIn += unit.payload.size();
}

void StreamPipeline::workerLoop()
{
    MediaUnit unit;
    while (m_running.load(std::memory_order_relaxed)) {
        if (!m_queue.pop(unit)) {
            break;
        }

        if (unit.kind == MediaUnit::Kind::Video) {
            handleVideoUnit(unit);
        } else {
            handleAudioUnit(unit);
        }
    }
}

} // namespace CBridge
