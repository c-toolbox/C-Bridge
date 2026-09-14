#include "core/bridgeengine.h"

#include "core/streampipeline.h"

#include <QTimer>

using namespace Qt::Literals::StringLiterals;

namespace CBridge {

namespace {
constexpr int kStatsIntervalMs = 1000;
}

BridgeEngine::BridgeEngine(QObject *parent)
    : QObject(parent)
    , m_statsTimer(new QTimer(this))
{
    m_statsTimer->setInterval(kStatsIntervalMs);
    connect(m_statsTimer, &QTimer::timeout, this, &BridgeEngine::sampleStats);
}

BridgeEngine::~BridgeEngine()
{
    stopAll();
}

void BridgeEngine::setConfig(const BridgeConfig &config)
{
    stopAll();
    m_config = config;
    m_stats.clear();
    m_lastBytesIn.clear();
}

void BridgeEngine::startAll()
{
    for (const StreamConfig &stream : m_config.streams) {
        if (stream.enabled) {
            startStream(stream.id);
        }
    }
    if (!m_pipelines.isEmpty()) {
        m_statsTimer->start();
    }
}

void BridgeEngine::startStream(const QString &streamId)
{
    if (m_pipelines.contains(streamId)) {
        return;
    }

    const int index = m_config.indexOfStream(streamId);
    if (index < 0) {
        return;
    }

    auto *pipeline = new StreamPipeline(m_config.streams.at(index), this);
    connect(pipeline, &StreamPipeline::stateChanged, this, &BridgeEngine::streamStateChanged);
    connect(pipeline, &StreamPipeline::errorOccurred, this, &BridgeEngine::streamError);

    m_pipelines.insert(streamId, pipeline);
    pipeline->start();
    m_statsTimer->start();
}

void BridgeEngine::stopStream(const QString &streamId)
{
    StreamPipeline *pipeline = m_pipelines.take(streamId);
    if (!pipeline) {
        return;
    }

    pipeline->stop();
    pipeline->deleteLater();

    if (m_pipelines.isEmpty()) {
        m_statsTimer->stop();
    }
}

void BridgeEngine::stopAll()
{
    const QStringList ids = m_pipelines.keys();
    for (const QString &id : ids) {
        stopStream(id);
    }
    m_statsTimer->stop();
}

void BridgeEngine::sampleStats()
{
    const double intervalSeconds = kStatsIntervalMs / 1000.0;

    for (auto it = m_pipelines.cbegin(); it != m_pipelines.cend(); ++it) {
        StreamStats stats = it.value()->stats();

        const quint64 previous = m_lastBytesIn.value(it.key(), stats.bytesIn);
        const quint64 delta = stats.bytesIn >= previous ? stats.bytesIn - previous : 0;
        m_lastBytesIn.insert(it.key(), stats.bytesIn);

        stats.inputMbps = (double(delta) * 8.0) / (intervalSeconds * 1'000'000.0);
        m_stats.insert(it.key(), stats);
    }

    Q_EMIT statsUpdated();
}

StreamStats BridgeEngine::statsFor(const QString &streamId) const
{
    return m_stats.value(streamId);
}

double BridgeEngine::aggregateInputMbps() const
{
    double total = 0.0;
    for (const StreamStats &stats : m_stats) {
        total += stats.inputMbps;
    }
    return total;
}

} // namespace CBridge
