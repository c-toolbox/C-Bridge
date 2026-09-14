#include "core/streampipeline.h"

#include "sinks/streamsink.h"
#include "sinks/tsmulticastsink.h"
#include "webrtc/webrtcsource.h"

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

void StreamPipeline::start()
{
    if (m_running.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    m_sawKeyframe = false;
    m_sinksOpen = false;
    m_reconnectDelayMs = m_config.reconnectInitialMs;

    for (const SinkConfig &sinkConfig : m_config.sinks) {
        if (!sinkConfig.enabled) {
            continue;
        }
        switch (sinkConfig.kind) {
        case SinkKind::TsMulticast:
            m_sinks.push_back(std::make_unique<TsMulticastSink>(sinkConfig.ts));
            break;
        case SinkKind::Ndi:
            // Added in the NDI phase; ignored here so a mixed config still runs.
            break;
        }
    }

    if (m_sinks.empty()) {
        Q_EMIT errorOccurred(m_config.id, u"No usable sink is configured"_s);
        setState(StreamState::Failed);
        m_running.store(false, std::memory_order_relaxed);
        return;
    }

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

    setState(StreamState::Idle);
}

bool StreamPipeline::openSinks()
{
    StreamFormat format;
    format.videoCodec = m_source ? m_source->negotiatedVideoCodec() : VideoCodec::Unknown;
    format.hasAudio = m_config.audioEnabled;

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
    return anyOpen;
}

void StreamPipeline::closeSinks()
{
    for (auto &sink : m_sinks) {
        sink->close();
    }
    m_sinksOpen = false;
}

void StreamPipeline::handleVideoUnit(const MediaUnit &unit)
{
    const BitstreamInspector::Result info =
        m_inspector.inspect(unit.payload.data(), unit.payload.size());

    if (info.width > 0 && info.height > 0) {
        std::lock_guard lock(m_statsMutex);
        m_stats.width = info.width;
        m_stats.height = info.height;
    }

    // Muxers need the parameter sets, so hold everything until the first keyframe.
    if (!m_sawKeyframe) {
        if (!info.isKeyframe || info.width == 0) {
            return;
        }
        m_sawKeyframe = true;
        if (!openSinks()) {
            return;
        }
    }

    for (auto &sink : m_sinks) {
        if (sink->isOpen()) {
            sink->writeVideo(unit.payload.data(), unit.payload.size(),
                             unit.rtpTimestamp, info.isKeyframe);
        }
    }

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

    for (auto &sink : m_sinks) {
        if (sink->isOpen()) {
            sink->writeAudio(unit.payload.data(), unit.payload.size(), unit.rtpTimestamp);
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
