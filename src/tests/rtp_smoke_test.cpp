/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Smoke test for the raw RTP multicast sink without a live WebRTC source or a real
// multicast group: it opens the sink against loopback unicast destinations (the rtp
// muxer over udp:// accepts any IP), feeds fake H.264 keyframes and Opus packets, and
// verifies that valid RTP datagrams arrive on both the video port and the audio port.

#include "sinks/rtpmulticastsink.h"

#include <QCoreApplication>
#include <QUdpSocket>
#include <QNetworkDatagram>

#include <cstdio>
#include <fstream>

using namespace Qt::Literals::StringLiterals;

namespace {

int g_failures = 0;

/// Prints a line and appends it to rtp_smoke_progress.log next to the executable, so
/// progress is visible even when stdout is lost (e.g. GUI-subsystem builds).
void logLine(const char *line)
{
    std::printf("%s\n", line);
    const QString file = QCoreApplication::applicationDirPath() + u"/rtp_smoke_progress.log"_s;
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

/// Parses the 12-byte RTP header of a datagram. Returns false for anything that is not
/// at least an RTP/RTCP version-2 packet (e.g. a runt).
bool rtpHeader(const QByteArray &datagram, int *payloadType, std::uint32_t *ssrc)
{
    if (datagram.size() < 12 || (int(datagram.at(0)) & 0xC0) != 0x80) {
        return false;
    }
    if (payloadType) {
        *payloadType = int(unsigned char(datagram.at(1)));
    }
    if (ssrc) {
        *ssrc = std::uint32_t(unsigned char(datagram.at(8))) << 24
            | std::uint32_t(unsigned char(datagram.at(9))) << 16
            | std::uint32_t(unsigned char(datagram.at(10))) << 8
            | std::uint32_t(unsigned char(datagram.at(11)));
    }
    return true;
}

/// Drains the socket, counting media datagrams and skipping RTCP (payload types 200..211).
void drainSocket(QUdpSocket &udp, int *mediaCount, std::uint32_t *ssrc, bool *ssrcConsistent)
{
    while (udp.hasPendingDatagrams()) {
        QNetworkDatagram datagram = udp.receiveDatagram();
        const QByteArray bytes = datagram.data();

        int payloadType = -1;
        std::uint32_t packetSsrc = 0;
        if (!rtpHeader(bytes, &payloadType, &packetSsrc)) {
            continue;
        }
        // RTCP packets ride on the same socket; their payload types are 200..211
        // (SR, RR, SDES, BYE, APP, RTPFB, PSFB, XR). Dynamic media payload types can be
        // above that range (FFmpeg's rtp muxer may start at 224), so only 200..211 is RTCP.
        if (payloadType >= 200 && payloadType <= 211) {
            continue;
        }

        ++*mediaCount;
        if (!ssrc) {
            continue; // only the video socket tracks SSRC consistency
        }
        if (*ssrc == 0) {
            *ssrc = packetSsrc;
        } else if (*ssrc != packetSsrc) {
            *ssrcConsistent = false;
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep progress visible if something hangs
    progress("main: starting");

    const int videoPort = 38900;
    const int audioPort = 38901;

    QUdpSocket videoUdp;
    QUdpSocket audioUdp;
    if (!videoUdp.bind(QHostAddress::LocalHost, videoPort) || !audioUdp.bind(QHostAddress::LocalHost, audioPort)) {
        logLine("FAIL: could not bind the test UDP sockets");
        return 1;
    }

    // The rtp muxer over udp:// accepts any destination IP, so loopback unicast stands
    // in for the multicast group and keeps the test independent of the network stack.
    CBridge::RtpMulticastSinkConfig config;
    config.groupAddress = u"127.0.0.1"_s;
    config.port = quint16(videoPort);
    config.ttl = 1;

    CBridge::RtpMulticastSink sink(config);

    // A minimal but fully valid baseline-profile H.264 Annex-B unit (16x16): SPS + PPS +
    // IDR slice with start codes, hand-assembled so the RTP packetizer's start-code scan
    // accepts it. The smoke test only needs the RTP path to flow; the pixels are
    // meaningless.
    static const std::uint8_t kH264Keyframe[] = {
        0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E, 0xFB, 0xC8, // SPS: baseline L3.0, 1 MB x 1 MB
        0x00, 0x00, 0x01, 0x68, 0xC6, 0x3C, 0x80,             // PPS: CAVLC, deblocking on
        0x00, 0x00, 0x01, 0x65, 0x98, 0x42, 0xE0              // IDR slice (I), poc_lsb 0
    };

    CBridge::StreamFormat format;
    format.videoCodec = CBridge::VideoCodec::H264;
    format.width = 1920;
    format.height = 1080;
    format.hasAudio = true;
    format.audioSampleRate = 48000;
    format.audioChannels = 2;

    // The extradata is the Annex-B parameter sets (SPS + PPS), as the pipeline provides.
    format.videoExtradata = QByteArray(reinterpret_cast<const char *>(kH264Keyframe), 15);

    progress("opening sink");
    QString error;
    if (!sink.open(format, &error)) {
        logLine(("FAIL: open: " + error).toUtf8().constData());
        return 1;
    }
    check(true, "sink opened (video and audio contexts)");

    // A plausible Opus packet (SILK-FB stereo TOC byte); the raw RTP path copies it
    // verbatim, so the payload content is irrelevant here.
    static const std::uint8_t kOpusPacket[] = { 0xFC, 0x01 };

    progress("feeding video and audio frames");
    int videoMedia = 0;
    int audioMedia = 0;
    std::uint32_t videoSsrc = 0;
    bool ssrcConsistent = true;

    std::uint32_t videoTimestamp = 0;
    std::uint32_t audioTimestamp = 0;
    for (int i = 0; i < 120 && (videoMedia == 0 || audioMedia == 0); ++i) {
        const bool keyframe = (i % 30) == 0;
        sink.writeVideo(kH264Keyframe, sizeof(kH264Keyframe), videoTimestamp, keyframe);
        videoTimestamp += 3000; // 30 fps on the 90 kHz RTP clock

        sink.writeAudio(kOpusPacket, sizeof(kOpusPacket), audioTimestamp);
        audioTimestamp += 960; // 20 ms frames on the 48 kHz Opus clock

        if (videoUdp.waitForReadyRead(100)) {
            drainSocket(videoUdp, &videoMedia, &videoSsrc, &ssrcConsistent);
        }
        if (audioUdp.hasPendingDatagrams()) {
            drainSocket(audioUdp, &audioMedia, nullptr, &ssrcConsistent);
        }
    }

    progress("closing sink");
    sink.close();

    // Drain anything that was flushed by the trailer rather than per packet, so the
    // checks below see everything the sink sent either way.
    if (videoUdp.waitForReadyRead(500)) {
        drainSocket(videoUdp, &videoMedia, &videoSsrc, &ssrcConsistent);
    }
    if (audioUdp.waitForReadyRead(500)) {
        drainSocket(audioUdp, &audioMedia, nullptr, &ssrcConsistent);
    }

    check(videoMedia > 0, "RTP video datagrams arrived on the video port");
    check(audioMedia > 0, "RTP audio datagrams arrived on the audio port");
    check(ssrcConsistent, "all video packets share one SSRC");
    check(sink.bytesWritten() > 0, "bytesWritten counts the fed frames");

    videoUdp.close();
    audioUdp.close();

    if (g_failures > 0) {
        logLine(("rtp-smoke-test: " + QString::number(g_failures) + u" failure(s)"_s).toUtf8().constData());
        return 1;
    }
    logLine("rtp-smoke-test: all checks passed");
    return 0;
}