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

ColumnLayout {
    id: form

    required property int sinkRow
    required property string senderName
    required property int targetWidth
    required property int targetHeight
    required property int fpsNum
    required property int fpsDen
    required property bool audioEnabled

    readonly property bool duplicateName: controller.draftProblems.some(function (problem) {
        return problem.indexOf("NDI sender name") === 0
    })

    spacing: Kirigami.Units.smallSpacing

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: !controller.ndiAvailable
        type: Kirigami.MessageType.Warning
        text: qsTr("%1 This sink will be skipped; multicast sinks are unaffected.")
            .arg(controller.ndiStatus)
    }

    Kirigami.FormLayout {
        Layout.fillWidth: true

        Controls.TextField {
            Kirigami.FormData.label: qsTr("Sender name:")
            text: form.senderName
            placeholderText: qsTr("Shown in NDI Studio Monitor")
            onTextEdited: controller.draft.sinks.set(form.sinkRow, "senderName", text)
        }

        Controls.Label {
            visible: form.duplicateName
            color: Kirigami.Theme.negativeTextColor
            text: qsTr("This sender name is already in use.")
        }

        Controls.SpinBox {
            Kirigami.FormData.label: qsTr("Target width:")
            from: 0
            to: 7680
            stepSize: 2
            value: form.targetWidth
            onValueModified: controller.draft.sinks.set(form.sinkRow, "targetWidth", value)
        }

        Controls.SpinBox {
            Kirigami.FormData.label: qsTr("Target height:")
            from: 0
            to: 4320
            stepSize: 2
            value: form.targetHeight
            onValueModified: controller.draft.sinks.set(form.sinkRow, "targetHeight", value)
        }

        Controls.SpinBox {
            Kirigami.FormData.label: qsTr("Frame rate numerator:")
            from: 0
            to: 240000
            value: form.fpsNum
            onValueModified: controller.draft.sinks.set(form.sinkRow, "fpsNum", value)
        }

        Controls.SpinBox {
            Kirigami.FormData.label: qsTr("Frame rate denominator:")
            from: 1
            to: 1001
            value: form.fpsDen
            onValueModified: controller.draft.sinks.set(form.sinkRow, "fpsDen", value)
        }

        Controls.Label {
            opacity: 0.6
            text: qsTr("0 keeps the source value.")
        }

        Controls.CheckBox {
            Kirigami.FormData.label: qsTr("Audio:")
            text: qsTr("Send audio")
            checked: form.audioEnabled
            onToggled: controller.draft.sinks.set(form.sinkRow, "ndiAudioEnabled", checked)
        }
    }
}
