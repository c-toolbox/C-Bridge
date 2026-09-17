/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Smoke test for the RTSP unicast sink without a live WebRTC source: it opens the
// sink, walks one client through OPTIONS/DESCRIBE/SETUP/PLAY over TCP, feeds fake
// video units and verifies that RTP datagrams actually arrive on the UDP port the
// client offered in SETUP.

#include "sinks/rtspsink.h"

#include <QCoreApplication>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QNetworkDatagram>

#include <cstdio>
#include <fstream>

using namespace Qt::Literals::StringLiterals;

namespace {

int g_failures = 0;

/// Prints a line and appends it to rtsp_smoke_progress.log next to the executable, so
/// progress is visible even when stdout is lost (e.g. GUI-subsystem builds).
void logLine(const char *line)
{
    std::printf("%s\n", line);
    const QString file = QCoreApplication::applicationDirPath() + u"/rtsp_smoke_progress.log"_s;
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

/// Sends one RTSP request and reads the full response (headers plus any body).
bool exchange(QTcpSocket &socket, const QByteArray &request, QString *response)
{
    socket.write(request);
    if (!socket.waitForBytesWritten(3000)) {
        return false;
    }

    QByteArray buffer = socket.readAll();
    for (;;) {
        const int end = buffer.indexOf("\r\n\r\n");
        if (end >= 0) {
            int bodyLength = 0;
            for (const QByteArray &line : buffer.left(end).split('\n')) {
                if (line.left(15).compare("Content-Length:", Qt::CaseInsensitive) == 0) {
                    bodyLength = line.mid(15).trimmed().toInt();
                }
            }
            while (buffer.size() < end + 4 + bodyLength) {
                if (!socket.waitForReadyRead(3000)) {
                    return false;
                }
                buffer.append(socket.readAll());
            }
            *response = QString::fromUtf8(buffer.left(end + 4 + bodyLength));
            return true;
        }
        if (!socket.waitForReadyRead(3000)) {
            return false;
        }
        buffer.append(socket.readAll());
    }
}

bool isOk(const QString &response)
{
    return response.startsWith(u"RTSP/1.0 200"_s);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep progress visible if something hangs
    progress("main: starting");

    const int rtspPort = 18554;
    const QString url = u"rtsp://127.0.0.1:%1/stream"_s.arg(rtspPort);

    CBridge::RtspSinkConfig config;
    config.port = rtspPort;
    config.path = u"stream"_s;

    CBridge::RtspSink sink(config, u"smoke-test"_s);

    CBridge::StreamFormat format;
    format.videoCodec = CBridge::VideoCodec::H264;
    format.width = 1920;
    format.height = 1080;

    progress("opening sink");
    QString error;
    if (!sink.open(format, &error)) {
        logLine(("FAIL: open: " + error).toUtf8().constData());
        return 1;
    }
    check(true, "sink opened");

    // The client's RTP receiver: an ephemeral UDP port on loopback.
    QUdpSocket udp;
    if (!udp.bind(QHostAddress::LocalHost, 0)) {
        logLine("FAIL: could not bind the test UDP socket");
        return 1;
    }
    const int clientPort = int(udp.localPort());

    progress("connecting to RTSP server");
    QTcpSocket tcp;
    tcp.connectToHost(QHostAddress::LocalHost, rtspPort);
    if (!tcp.waitForConnected(3000)) {
        logLine("FAIL: could not connect to the RTSP server");
        return 1;
    }

    QString response;
    progress("sending OPTIONS");
    const QByteArray options = u"OPTIONS %1 RTSP/1.0\r\nCSeq: 1\r\n\r\n"_s.arg(url).toUtf8();
    check(exchange(tcp, options, &response) && isOk(response), "OPTIONS");

    progress("sending DESCRIBE");
    const QByteArray describe = u"DESCRIBE %1 RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n"_s.arg(url).toUtf8();
    check(exchange(tcp, describe, &response) && isOk(response), "DESCRIBE");
    check(response.contains(u"MP2T/90000"_s) && response.contains(u"m=video"_s),
          "SDP advertises MP2T over RTP payload 33");

    progress("sending SETUP");
    const QByteArray setup =
        (u"SETUP %1 RTSP/1.0\r\nCSeq: 3\r\nTransport: RTP/AVP;unicast;client_port=%2\r\n\r\n"_s
            .arg(url)
            .arg(clientPort))
            .toUtf8();
    check(exchange(tcp, setup, &response) && isOk(response), "SETUP");

    progress("sending PLAY");
    const QByteArray play = u"PLAY %1 RTSP/1.0\r\nCSeq: 4\r\nSession: x\r\n\r\n"_s.arg(url).toUtf8();
    check(exchange(tcp, play, &response) && isOk(response), "PLAY");

    // A minimal but fully valid baseline-profile H.264 Annex-B unit (16x16): SPS + PPS +
    // IDR slice with start codes, hand-assembled so the mpegts muxer's bitstream parser
    // accepts it. The smoke test only needs the RTP path to flow; the pixels are
    // meaningless.
    static const std::uint8_t kH264Keyframe[] = {
        0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1E, 0xFB, 0xC8, // SPS: baseline L3.0, 1 MB x 1 MB
        0x00, 0x00, 0x01, 0x68, 0xC6, 0x3C, 0x80,             // PPS: CAVLC, deblocking on
        0x00, 0x00, 0x01, 0x65, 0x98, 0x42, 0xE0              // IDR slice (I), poc_lsb 0
    };

    progress("feeding video frames");
    std::uint32_t rtpTimestamp = 0;
    bool gotDatagram = false;
    for (int i = 0; i < 60 && !gotDatagram; ++i) {
        sink.writeVideo(kH264Keyframe, sizeof(kH264Keyframe), rtpTimestamp, /*isKeyframe=*/true);
        rtpTimestamp += 3000; // 30 fps on the 90 kHz RTP clock

        if (udp.waitForReadyRead(250)) {
            while (udp.hasPendingDatagrams()) {
                udp.receiveDatagram();
                gotDatagram = true;
            }
        }
    }
    check(gotDatagram, "RTP datagrams arrived on the client UDP port");

    progress("sending TEARDOWN");
    const QByteArray teardown = u"TEARDOWN %1 RTSP/1.0\r\nCSeq: 5\r\nSession: x\r\n\r\n"_s.arg(url).toUtf8();
    check(exchange(tcp, teardown, &response) && isOk(response), "TEARDOWN");

    progress("closing sink");
    sink.close();
    tcp.disconnectFromHost();
    udp.close();

    if (g_failures > 0) {
        logLine(("rtsp-smoke-test: " + QString::number(g_failures) + u" failure(s)"_s).toUtf8().constData());
        return 1;
    }
    logLine("rtsp-smoke-test: all checks passed");
    return 0;
}
