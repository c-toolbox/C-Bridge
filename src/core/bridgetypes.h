#pragma once

#include <QMetaType>
#include <QString>

namespace CBridge {

Q_NAMESPACE

enum class VideoCodec {
    Unknown,
    H264,
    H265,
};
Q_ENUM_NS(VideoCodec)

enum class AudioCodec {
    Unknown,
    Opus,
};
Q_ENUM_NS(AudioCodec)

enum class SinkKind {
    TsMulticast,
    Ndi,
};
Q_ENUM_NS(SinkKind)

enum class StreamState {
    Idle,
    Connecting,
    Running,
    Retrying,
    Failed,
    Stopping,
};
Q_ENUM_NS(StreamState)

QString toString(VideoCodec codec);
VideoCodec videoCodecFromString(const QString &text);

QString toString(SinkKind kind);
SinkKind sinkKindFromString(const QString &text, bool *ok = nullptr);

QString toString(StreamState state);

/// Per-stream counters sampled by the engine once a second for the UI.
struct StreamStats {
    StreamState state = StreamState::Idle;

    int width = 0;
    int height = 0;
    double sourceFps = 0.0;
    VideoCodec videoCodec = VideoCodec::Unknown;

    quint64 videoFramesIn = 0;
    quint64 audioFramesIn = 0;
    quint64 bytesIn = 0;

    double inputMbps = 0.0;
    double outputMbps = 0.0;

    quint64 droppedFrames = 0;
    int queueDepth = 0;
    int reconnectCount = 0;

    qint64 lastFrameEpochMs = 0;
    QString lastError;
};

} // namespace CBridge

Q_DECLARE_METATYPE(CBridge::StreamStats)
