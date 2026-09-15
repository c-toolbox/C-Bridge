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
    required property string groupAddress
    required property int port
    required property int ttl
    required property string localAddress
    required property int packetSize
    required property int patPeriodMs
    required property int pcrPeriodMs
    required property string tsUrl

    RowLayout {
        Kirigami.FormData.label: qsTr("Group address:")

        Controls.TextField {
            text: form.groupAddress
            placeholderText: "239.1.1.1"
            onTextEdited: controller.draft.sinks.set(form.sinkRow, "groupAddress", text)
        }

        Controls.Button {
            text: qsTr("Suggest free address")
            onClicked: controller.suggestMulticastFor(form.sinkRow)
        }
    }

    Controls.SpinBox {
        Kirigami.FormData.label: qsTr("Port:")
        from: 1
        to: 65535
        value: form.port
        onValueModified: controller.draft.sinks.set(form.sinkRow, "port", value)
    }

    Controls.SpinBox {
        Kirigami.FormData.label: qsTr("TTL:")
        from: 1
        to: 255
        value: form.ttl
        onValueModified: controller.draft.sinks.set(form.sinkRow, "ttl", value)
    }

    Controls.TextField {
        Kirigami.FormData.label: qsTr("Local interface:")
        text: form.localAddress
        placeholderText: qsTr("Empty lets the OS choose")
        onTextEdited: controller.draft.sinks.set(form.sinkRow, "localAddress", text)
    }

    Controls.SpinBox {
        Kirigami.FormData.label: qsTr("Packet size:")
        from: 188
        to: 65535
        stepSize: 188
        value: form.packetSize
        onValueModified: controller.draft.sinks.set(form.sinkRow, "packetSize", value)
    }

    Controls.SpinBox {
        Kirigami.FormData.label: qsTr("PAT period (ms):")
        from: 10
        to: 5000
        stepSize: 10
        value: form.patPeriodMs
        onValueModified: controller.draft.sinks.set(form.sinkRow, "patPeriodMs", value)
    }

    Controls.SpinBox {
        Kirigami.FormData.label: qsTr("PCR period (ms):")
        from: 10
        to: 500
        stepSize: 10
        value: form.pcrPeriodMs
        onValueModified: controller.draft.sinks.set(form.sinkRow, "pcrPeriodMs", value)
    }

    Controls.TextField {
        Kirigami.FormData.label: qsTr("Output URL:")
        readOnly: true
        text: form.tsUrl
    }
}
