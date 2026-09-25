/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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
    m_sinkStats.clear();
}

void BridgeEngine::setStreamPassword(const QString &streamId, const QString &password)
{
    if (password.isEmpty()) {
        m_streamPasswords.remove(streamId);
        return;
    }
    m_streamPasswords.insert(streamId, password);
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

    // The URL may carry no embedded credentials; the resolved password was pushed by the
    // controller before start.
    const QString password = m_streamPasswords.value(streamId);
    if (!password.isEmpty()) {
        pipeline->setPassword(password);
    }

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

    // Drop the counters before stop() so the Idle emission re-reads a clean state
    // instead of the last sampled one, which still says Running.
    m_stats.remove(streamId);
    m_lastBytesIn.remove(streamId);
    m_sinkStats.remove(streamId);

    pipeline->stop();
    pipeline->deleteLater();

    // stop() may return early without emitting (e.g. a stream that never got running),
    // so the model is told explicitly to pick up the idle state.
    Q_EMIT streamStateChanged(streamId, StreamState::Idle);

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

        QList<SinkStats> sinks = it.value()->sinkStats();
        const QList<SinkStats> previousSinks = m_sinkStats.value(it.key());
        double streamOutputMbps = 0.0;
        for (int i = 0; i < sinks.size(); ++i) {
            const quint64 before = i < previousSinks.size() ? previousSinks.at(i).bytesWritten
                                                            : sinks.at(i).bytesWritten;
            const quint64 sinkDelta =
                sinks.at(i).bytesWritten >= before ? sinks.at(i).bytesWritten - before : 0;
            sinks[i].outputMbps = (double(sinkDelta) * 8.0) / (intervalSeconds * 1'000'000.0);
            streamOutputMbps += sinks.at(i).outputMbps;
        }
        m_sinkStats.insert(it.key(), sinks);

        stats.outputMbps = streamOutputMbps;
        m_stats.insert(it.key(), stats);
    }

    Q_EMIT statsUpdated();
}

StreamStats BridgeEngine::statsFor(const QString &streamId) const
{
    return m_stats.value(streamId);
}

QList<SinkStats> BridgeEngine::sinkStatsFor(const QString &streamId) const
{
    return m_sinkStats.value(streamId);
}

double BridgeEngine::aggregateInputMbps() const
{
    double total = 0.0;
    for (const StreamStats &stats : m_stats) {
        total += stats.inputMbps;
    }
    return total;
}

double BridgeEngine::aggregateOutputMbps() const
{
    double total = 0.0;
    for (const StreamStats &stats : m_stats) {
        total += stats.outputMbps;
    }
    return total;
}

} // namespace CBridge
