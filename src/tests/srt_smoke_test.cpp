/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Smoke test for the SRT source without a live encoder, in two parts:
// 1. Regression check that an empty-host listener binds and waits for a peer instead of
//    failing immediately. srt://:<port> used to resolve to the IPv6 wildcard on dual-stack
//    Windows hosts, which this FFmpeg/SRT build refuses with SRT_EINVOP because SRTO_IPV6ONLY
//    is not set; the source now binds 0.0.0.0 explicitly.
// 2. Full loopback round-trip: an in-process FFmpeg SRT caller (mpegts muxer, H.264 keyframes,
//    streamid "smoketest") dials the production SrtSource listener and video units must come
//    back through its callback with the negotiated codec set to H.264.

#include "srt/srtsource.h"
#include "media/audiodecoder.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <QCoreApplication>
#include <QMutex>
#include <QString>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <thread>
#include <vector>

using namespace Qt::Literals::StringLiterals;

// The production build gets this from cudacontext.cpp, which the smoke test does not link, so
// provide its own copy of the shared helper declared in avwrappers.h (AudioDecoder uses it).
namespace CBridge {

QString avErrorString(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

} // namespace CBridge

namespace {

int g_failures = 0;

/// Prints a line and appends it to srt_smoke_progress.log next to the executable, so progress
/// is visible even when stdout is lost (e.g. GUI-subsystem builds).
void logLine(const char *line)
{
    std::printf("%s\n", line);
    const QString file = QCoreApplication::applicationDirPath() + u"/srt_smoke_progress.log"_s;
    std::ofstream log(file.toUtf8().constData(), std::ios::app);
    log << line << "\n";
}

void progress(const char *what)
{
    logLine(what);
}

void check(bool ok, const char *what)
{
    char line[256];
    std::snprintf(line, sizeof(line), "%s: %s", ok ? "PASS" : "FAIL", what);
    logLine(line);
    if (!ok) {
        ++g_failures;
    }
}

/// A minimal but fully valid baseline-profile H.264 Annex-B unit (16x16): SPS + PPS + IDR
/// slice with start codes, hand-assembled so the mpegts muxer's bitstream parser accepts it.
/// The smoke test only needs the SRT path to flow; the pixels are meaningless.
static const std::uint8_t kH264Keyframe[] = {
    0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E, 0xFB, 0xC8, // SPS: baseline L3.0, 1 MB x 1 MB
    0x00, 0x00, 0x01, 0x68, 0xC6, 0x3C, 0x80,             // PPS: CAVLC, deblocking on
    0x00, 0x00, 0x01, 0x65, 0x98, 0x42, 0xE0              // IDR slice (I), poc_lsb 0
};

/// In-process SRT caller: an mpegts muxer writing H.264 keyframes to the listener at `url`,
/// standing in for an encoder pushing into MediaMTX's SRT port. The streamid is passed as an
/// AVDictionary option, exactly like the production caller path does.
class SrtCallerWriter {
public:
    bool start(const QString &url, const QByteArray &streamId)
    {
        AVDictionary *options = nullptr;
        av_dict_set(&options, "mode", "caller", 0);
        if (!streamId.isEmpty()) {
            av_dict_set(&options, "streamid", streamId.constData(), 0);
        }

        const QByteArray utf8Url = url.toUtf8();
        int ret = avformat_alloc_output_context2(&m_out, nullptr, u"mpegts"_s.toUtf8().constData(),
                                                 utf8Url.constData());
        if (ret < 0 || !m_out) {
            return false;
        }

        AVStream *stream = avformat_new_stream(m_out, nullptr);
        stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        stream->codecpar->codec_id = AV_CODEC_ID_H264;
        stream->time_base = AVRational { 1, 90000 };

        ret = avio_open2(&m_out->pb, utf8Url.constData(), AVIO_FLAG_WRITE, nullptr, &options);
        if (ret < 0) {
            return false;
        }
        ret = avformat_write_header(m_out, nullptr);
        if (ret < 0) {
            return false;
        }

        m_thread = std::thread([this] { run(); });
        return true;
    }

    void stop()
    {
        m_stop.store(true, std::memory_order_relaxed);
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    void run()
    {
        int64_t dts = 0;
        while (!m_stop.load(std::memory_order_relaxed)) {
            AVPacket packet {};
            packet.data = const_cast<uint8_t *>(kH264Keyframe);
            packet.size = sizeof(kH264Keyframe);
            packet.dts = dts;
            packet.pts = dts;
            if (av_interleaved_write_frame(m_out, &packet) < 0) {
                break; // the listener went away
            }
            dts += 3000; // 30 fps on the 90 kHz clock
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }

        av_write_trailer(m_out);
        if (m_out->pb) {
            avio_closep(&m_out->pb);
        }
        avformat_free_context(m_out);
        m_out = nullptr;
    }

    AVFormatContext *m_out = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop { false };
};

/// Polls the source until it reaches `wanted` or 10 s pass, so a regression fails fast but
/// with plenty of margin on slow machines.
bool waitForState(CBridge::StreamSource &source, CBridge::StreamState wanted)
{
    for (int i = 0; i < 100; ++i) {
        if (source.state() == wanted) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return source.state() == wanted;
}

/// Encodes a 440 Hz stereo tone into raw AAC frames with FFmpeg's native encoder, exactly
/// the elementary-stream packets an encoder pushes into the mpegts muxer (which then adds
/// the ADTS headers the SrtSource demuxes back as AV_CODEC_ID_AAC).
bool encodeAacTone(std::vector<std::vector<std::uint8_t>> &packets, QString *error)
{
    constexpr int kRate = 48000;
    constexpr int kChannels = 2;
    constexpr int kFrameSize = 1024; // the AAC frame size
    constexpr int kTotalSamples = kRate * 2;

    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!encoder) {
        if (error) {
            *error = u"No native AAC encoder in this FFmpeg build"_s;
        }
        return false;
    }

    CBridge::CodecContextPtr context(avcodec_alloc_context3(encoder));
    if (!context) {
        return false;
    }
    context->sample_rate = kRate;
    av_channel_layout_default(&context->ch_layout, kChannels);
    context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    context->bit_rate = 128000;
    context->frame_size = kFrameSize;
    context->time_base = AVRational { 1, kRate };
    if (avcodec_open2(context.get(), encoder, nullptr) < 0) {
        if (error) {
            *error = u"Could not open the native AAC encoder"_s;
        }
        return false;
    }

    CBridge::FramePtr frame(CBridge::makeFrame());
    CBridge::PacketPtr packet(CBridge::makePacket());
    auto encodeOne = [&](AVFrame *toSend) -> bool {
        if (avcodec_send_frame(context.get(), toSend) < 0) {
            return false;
        }
        while (true) {
            const int ret = avcodec_receive_packet(context.get(), packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }
            if (ret < 0) {
                return false;
            }
            packets.emplace_back(packet->data, packet->data + packet->size);
            av_packet_unref(packet.get());
        }
    };

    for (int offset = 0; offset < kTotalSamples; offset += kFrameSize) {
        av_frame_unref(frame.get());
        frame->format = AV_SAMPLE_FMT_FLTP;
        frame->sample_rate = kRate;
        av_channel_layout_default(&frame->ch_layout, kChannels);
        frame->nb_samples = kFrameSize;
        frame->pts = offset;
        if (av_frame_get_buffer(frame.get(), 0) < 0) {
            return false;
        }
        for (int i = 0; i < kFrameSize; ++i) {
            const double t = double(offset + i) / kRate;
            const float value = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * t));
            for (int c = 0; c < kChannels; ++c) {
                reinterpret_cast<float *>(frame->data[c])[i] = value;
            }
        }
        if (!encodeOne(frame.get())) {
            return false;
        }
    }
    encodeOne(nullptr); // flush

    if (packets.empty()) {
        if (error) {
            *error = u"The AAC encoder produced no packets"_s;
        }
        return false;
    }
    return true;
}

/// An SRT caller that muxes H.264 keyframes and the AAC tone into MPEG-TS and dials the
/// production listener, standing in for an encoder pushing AAC audio into MediaMTX.
class SrtCallerWriterAac {
public:
    bool start(const QString &url, const QByteArray &streamId,
               const std::vector<std::vector<std::uint8_t>> &audioPackets)
    {
        AVDictionary *options = nullptr;
        av_dict_set(&options, "mode", "caller", 0);
        if (!streamId.isEmpty()) {
            av_dict_set(&options, "streamid", streamId.constData(), 0);
        }
        m_audioPackets = &audioPackets;

        const QByteArray utf8Url = url.toUtf8();
        int ret = avformat_alloc_output_context2(&m_out, nullptr, u"mpegts"_s.toUtf8().constData(),
                                                 utf8Url.constData());
        if (ret < 0 || !m_out) {
            return false;
        }

        m_videoStream = avformat_new_stream(m_out, nullptr);
        m_videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        m_videoStream->codecpar->codec_id = AV_CODEC_ID_H264;
        m_videoStream->time_base = AVRational { 1, 90000 };

        m_audioStream = avformat_new_stream(m_out, nullptr);
        m_audioStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        m_audioStream->codecpar->codec_id = AV_CODEC_ID_AAC;
        m_audioStream->codecpar->sample_rate = 48000;
        av_channel_layout_default(&m_audioStream->codecpar->ch_layout, 2);
        m_audioStream->time_base = AVRational { 1, 48000 };
        // The mpegts muxer synthesises the ADTS headers from this AudioSpecificConfig
        // (AAC-LC, 48 kHz, stereo = 0x11 0x90), matching what a real encoder emits.
        static const std::uint8_t kAsc[] = { 0x11, 0x90 };
        m_audioStream->codecpar->extradata = static_cast<uint8_t *>(
            av_mallocz(sizeof(kAsc) + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(m_audioStream->codecpar->extradata, kAsc, sizeof(kAsc));
        m_audioStream->codecpar->extradata_size = int(sizeof(kAsc));

        ret = avio_open2(&m_out->pb, utf8Url.constData(), AVIO_FLAG_WRITE, nullptr, &options);
        if (ret < 0) {
            return false;
        }
        ret = avformat_write_header(m_out, nullptr);
        if (ret < 0) {
            return false;
        }

        m_thread = std::thread([this] { run(); });
        return true;
    }

    void stop()
    {
        m_stop.store(true, std::memory_order_relaxed);
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    void run()
    {
        int64_t videoDts = 0;
        int64_t audioDts = 0;
        std::size_t audioIndex = 0;
        while (!m_stop.load(std::memory_order_relaxed)) {
            AVPacket video {};
            video.data = const_cast<uint8_t *>(kH264Keyframe);
            video.size = sizeof(kH264Keyframe);
            video.stream_index = m_videoStream->index;
            video.dts = video.pts = videoDts;
            video.flags = AV_PKT_FLAG_KEY;
            if (av_interleaved_write_frame(m_out, &video) < 0) {
                break;
            }
            videoDts += 3000; // 30 fps on the 90 kHz clock

            if (audioIndex < m_audioPackets->size()) {
                const std::vector<std::uint8_t> &raw = (*m_audioPackets)[audioIndex++];
                AVPacket audio {};
                if (av_new_packet(&audio, int(raw.size())) < 0) {
                    break;
                }
                std::memcpy(audio.data, raw.data(), raw.size());
                audio.stream_index = m_audioStream->index;
                audio.dts = audio.pts = audioDts;
                audioDts += 1024; // one AAC frame on the 48 kHz clock
                av_interleaved_write_frame(m_out, &audio);
                av_packet_unref(&audio);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(21));
        }

        av_write_trailer(m_out);
        if (m_out->pb) {
            avio_closep(&m_out->pb);
        }
        avformat_free_context(m_out);
        m_out = nullptr;
    }

    AVFormatContext *m_out = nullptr;
    AVStream *m_videoStream = nullptr;
    AVStream *m_audioStream = nullptr;
    const std::vector<std::vector<std::uint8_t>> *m_audioPackets = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop { false };
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep progress visible if something hangs
    progress("main: starting");

    // --- Part A: empty-host listener must bind and wait for a peer --------------
    {
        const int port = 19000;
        CBridge::StreamConfig config {};
        config.srt.mode = CBridge::SrtSourceConfig::Mode::Listener;
        config.srt.host.clear(); // the regression: an empty host used to build srt://:<port>
        config.srt.port = quint16(port);

        progress("starting listener with an empty bind host");
        CBridge::SrtSource source;
        QMutex errorMutex;
        QString errorMessage;
        QObject::connect(&source, &CBridge::StreamSource::errorOccurred,
                         [&errorMessage, &errorMutex](const QString &message) {
                             QMutexLocker locker(&errorMutex);
                             errorMessage = message;
                         });

        source.setConfig(config);
        source.start();

        // The old code failed immediately here (SRT_EINVOP on the IPv6 wildcard bind). Give it
        // a few seconds: a healthy listener is still blocked in accept, i.e. Connecting.
        bool stayedConnecting = false;
        for (int i = 0; i < 50 && !stayedConnecting; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const CBridge::StreamState state = source.state();
            if (state == CBridge::StreamState::Failed || state == CBridge::StreamState::Idle) {
                break; // died right away - the regression is back
            }
            stayedConnecting = (state == CBridge::StreamState::Connecting);
        }
        check(stayedConnecting, "empty-host listener binds and waits for a peer");

        progress("stopping listener");
        source.stop();
        {
            QMutexLocker locker(&errorMutex);
            if (!errorMessage.isEmpty()) {
                logLine(("listener error: " + errorMessage).toUtf8().constData());
            }
        }
    }

    // --- Part B: loopback round-trip through the production listener ------------
    {
        const int port = 19001;

        CBridge::StreamConfig config {};
        config.srt.mode = CBridge::SrtSourceConfig::Mode::Listener;
        config.srt.port = quint16(port);
        config.srt.latencyMs = 50; // keep the round-trip snappy

        progress("starting listener for the loopback caller");
        CBridge::SrtSource source;
        QMutex errorMutex;
        QString errorMessage;
        QObject::connect(&source, &CBridge::StreamSource::errorOccurred,
                         [&errorMessage, &errorMutex](const QString &message) {
                             QMutexLocker locker(&errorMutex);
                             errorMessage = message;
                         });

        std::atomic<int> videoUnits { 0 };
        source.setVideoCallback(
            [&videoUnits](const std::uint8_t *, std::size_t, quint32, quint32) { ++videoUnits; });

        source.setConfig(config);
        source.start();

        // Let the listener finish binding before the caller dials in.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        progress("starting in-process SRT caller (mpegts, streamid=smoketest)");
        SrtCallerWriter writer;
        if (!writer.start(u"srt://127.0.0.1:%1"_s.arg(port), u"smoketest"_s.toUtf8())) {
            logLine("FAIL: could not start the in-process SRT caller");
            source.stop();
            return 1;
        }

        check(waitForState(source, CBridge::StreamState::Running),
              "listener accepted the caller and reached Running");
        check(source.negotiatedVideoCodec() == CBridge::VideoCodec::H264,
              "negotiated codec is H.264");

        bool gotVideo = false;
        for (int i = 0; i < 100 && !gotVideo; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            gotVideo = videoUnits.load() > 0;
        }
        check(gotVideo, "video units arrived through the SRT source callback");

        progress("stopping loopback");
        source.stop();
        writer.stop();
        {
            QMutexLocker locker(&errorMutex);
            if (!errorMessage.isEmpty()) {
                logLine(("loopback error: " + errorMessage).toUtf8().constData());
            }
        }
    }

    // --- Part C: AAC audio round-trip through the production listener ----------
    {
        const int port = 19002;

        QString encodeError;
        std::vector<std::vector<std::uint8_t>> aacPackets;
        if (!encodeAacTone(aacPackets, &encodeError)) {
            logLine((u"FAIL: could not encode the AAC test tone: " + encodeError)
                        .toUtf8()
                        .constData());
            ++g_failures;
        } else {
            CBridge::StreamConfig config {};
            config.srt.mode = CBridge::SrtSourceConfig::Mode::Listener;
            config.srt.port = quint16(port);
            config.srt.latencyMs = 50;

            progress("starting listener for the AAC loopback caller");
            CBridge::SrtSource source;
            QMutex errorMutex;
            QString errorMessage;
            QObject::connect(&source, &CBridge::StreamSource::errorOccurred,
                             [&errorMessage, &errorMutex](const QString &message) {
                                 QMutexLocker locker(&errorMutex);
                                 errorMessage = message;
                             });

            std::atomic<int> videoUnits { 0 };
            source.setVideoCallback([&videoUnits](const std::uint8_t *, std::size_t, quint32,
                                                  quint32) {
                ++videoUnits;
            });

            // Feed every arriving audio packet into the production AudioDecoder, exactly
            // like the pipeline's NDI path does for AAC.
            CBridge::AudioDecoder decoder;
            std::atomic<int> audioUnits { 0 };
            std::atomic<int> decodedFrames { 0 };
            std::atomic<bool> decoderOpened { false };
            source.setAudioCallback([&](const std::uint8_t *data, std::size_t size, quint32 ts) {
                ++audioUnits;
                if (!decoderOpened.load()) {
                    QString openError;
                    if (!decoder.open(source.negotiatedAudioCodec(),
                                      source.negotiatedAudioSampleRate(),
                                      source.negotiatedAudioChannels(),
                                      source.negotiatedAudioExtradata(), &openError)) {
                        logLine(("FAIL: AAC decoder open: " + openError).toUtf8().constData());
                        ++g_failures;
                        decoderOpened.store(true); // do not retry every packet
                        return;
                    }
                    decoder.setFrameCallback(
                        [&decodedFrames](AVFrame *) { ++decodedFrames; });
                    decoderOpened.store(true);
                }
                QString decodeError;
                decoder.decode(data, size, std::int64_t(ts), &decodeError);
            });

            source.setConfig(config);
            source.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            progress("starting in-process AAC SRT caller (mpegts, H.264 + AAC)");
            SrtCallerWriterAac writer;
            if (!writer.start(u"srt://127.0.0.1:%1"_s.arg(port), u"smoketest"_s.toUtf8(),
                              aacPackets)) {
                logLine("FAIL: could not start the AAC SRT caller");
                source.stop();
                ++g_failures;
            } else {
                check(waitForState(source, CBridge::StreamState::Running),
                      "AAC: listener accepted the caller and reached Running");
                check(source.negotiatedAudioCodec() == CBridge::AudioCodec::Aac,
                      "AAC: negotiated audio codec is AAC");

                bool gotAudio = false;
                for (int i = 0; i < 100 && !gotAudio; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    gotAudio = audioUnits.load() > 0;
                }
                check(gotAudio, "AAC: audio units arrived through the SRT source callback");
                check(videoUnits.load() > 0, "AAC: video units arrived too");

                // Let the tone drain through the decoder before checking.
                for (int i = 0; i < 30 && decodedFrames.load() == 0; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                check(decodedFrames.load() > 0,
                      "AAC: the production AudioDecoder decoded the ADTS stream");

                progress("stopping AAC loopback");
                source.stop();
                writer.stop();
                {
                    QMutexLocker locker(&errorMutex);
                    if (!errorMessage.isEmpty()) {
                        logLine(("AAC loopback error: " + errorMessage).toUtf8().constData());
                    }
                }
            }
        }
    }

    if (g_failures > 0) {
        logLine((u"srt-smoke-test: %1 failure(s)"_s.arg(g_failures)).toUtf8().constData());
        return 1;
    }
    logLine("srt-smoke-test: all checks passed");
    return 0;
}

