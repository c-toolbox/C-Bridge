/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    id: form

    required property int sinkRow
    required property string rtpGroupAddress
    required property int rtpPort
    required property int rtpTtl
    required property string rtpLocalAddress
    required property int rtpPacketSize
    required property string rtpVideoUrl
    required property string rtpAudioUrl

    RowLayout {
        Kirigami.FormData.label: qsTr("Group address:")

        Controls.TextField {
            text: form.rtpGroupAddress
            placeholderText: "239.1.1.1"
            onTextEdited: controller.draft.sinks.set(form.sinkRow, "rtpGroupAddress", text)
        }

        Controls.Button {
            text: qsTr("Suggest free address")
            onClicked: controller.suggestMulticastFor(form.sinkRow)
        }
    }

    Controls.SpinBox {
        Kirigami.FormData.label: qsTr("Video port:")
        from: 1
        to: 65534 // the audio stream uses the next port
        value: form.rtpPort
        onValueModified: controller.draft.sinks.set(form.sinkRow, "rtpPort", value)
    }

    Controls.SpinBox {
        Kirigami.FormData.label: qsTr("TTL:")
        from: 1
        to: 255
        value: form.rtpTtl
        onValueModified: controller.draft.sinks.set(form.sinkRow, "rtpTtl", value)
    }

    Controls.TextField {
        Kirigami.FormData.label: qsTr("Local interface:")
        text: form.rtpLocalAddress
        placeholderText: qsTr("Empty lets the OS choose")
        onTextEdited: controller.draft.sinks.set(form.sinkRow, "rtpLocalAddress", text)
    }

    Controls.SpinBox {
        Kirigami.FormData.label: qsTr("Packet size:")
        from: 64
        to: 65535
        value: form.rtpPacketSize
        onValueModified: controller.draft.sinks.set(form.sinkRow, "rtpPacketSize", value)
    }

    Controls.TextField {
        Kirigami.FormData.label: qsTr("Video URL:")
        readOnly: true
        text: form.rtpVideoUrl
    }

    Controls.TextField {
        Kirigami.FormData.label: qsTr("Audio URL:")
        readOnly: true
        text: form.rtpAudioUrl
    }

    Kirigami.Heading {
        level: 5
        Layout.fillWidth: true
        opacity: 0.7
        wrapMode: Text.WordWrap
        text: qsTr("Raw RTP over UDP multicast: H.264/H.265 video on the port above and Opus audio " +
                   "on the next one, with RTCP sender reports on the same sockets. Payload types and " +
                   "SSRCs are assigned dynamically by FFmpeg, so receivers should probe or be given an SDP.")
    }
}