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
    required property int rtspPort
    required property string rtspPath
    required property string rtspLocalAddress
    required property string rtspUrl

    Controls.SpinBox {
        Kirigami.FormData.label: qsTr("RTSP port:")
        from: 1
        to: 65535
        value: form.rtspPort
        onValueModified: controller.draft.sinks.set(form.sinkRow, "rtspPort", value)
    }

    Controls.TextField {
        Kirigami.FormData.label: qsTr("Path:")
        text: form.rtspPath
        placeholderText: "stream"
        onTextEdited: controller.draft.sinks.set(form.sinkRow, "rtspPath", text)
    }

    Controls.TextField {
        Kirigami.FormData.label: qsTr("Local interface:")
        text: form.rtspLocalAddress
        placeholderText: qsTr("Empty lets the OS choose")
        onTextEdited: controller.draft.sinks.set(form.sinkRow, "rtspLocalAddress", text)
    }

    Controls.TextField {
        Kirigami.FormData.label: qsTr("Player URL:")
        readOnly: true
        text: form.rtspUrl
    }

    Kirigami.Heading {
        level: 5
        Layout.fillWidth: true
        opacity: 0.7
        wrapMode: Text.WordWrap
        text: qsTr("Players open the URL above with RTSP unicast (VLC, ffplay -rtsp_transport udp). " +
                   "Each player gets its own MPEG-TS over UDP; no multicast is involved.")
    }
}
