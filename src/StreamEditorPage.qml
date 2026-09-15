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

Kirigami.ScrollablePage {
    id: page

    title: controller.draftIsNew ? qsTr("New stream") : qsTr("Edit stream")

    actions: [
        Kirigami.Action {
            text: qsTr("Save")
            icon.name: "document-save"
            enabled: controller.draftValid
            onTriggered: {
                if (controller.commitEdit())
                    pageStack.pop()
            }
        },
        Kirigami.Action {
            text: qsTr("Cancel")
            icon.name: "dialog-cancel"
            onTriggered: {
                controller.cancelEdit()
                pageStack.pop()
            }
        }
    ]

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: controller.draftProblems.length > 0
            type: Kirigami.MessageType.Warning
            text: controller.draftProblems.join("\n")
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true

            Controls.TextField {
                Kirigami.FormData.label: qsTr("Name:")
                text: controller.draft.name
                onTextEdited: controller.draft.name = text
            }

            Controls.TextField {
                Kirigami.FormData.label: qsTr("WHEP URL:")
                text: controller.draft.whepUrl
                placeholderText: "http://localhost:8889/mystream/whep"
                onTextEdited: controller.draft.whepUrl = text
            }

            Controls.TextField {
                Kirigami.FormData.label: qsTr("Username:")
                text: controller.draft.username
                onTextEdited: controller.draft.username = text
            }

            Controls.CheckBox {
                Kirigami.FormData.label: qsTr("Stream:")
                text: qsTr("Enabled")
                checked: controller.draft.enabled
                onToggled: controller.draft.enabled = checked
            }

            Controls.CheckBox {
                text: qsTr("Receive audio")
                checked: controller.draft.audioEnabled
                onToggled: controller.draft.audioEnabled = checked
            }

            Controls.ComboBox {
                Kirigami.FormData.label: qsTr("Preferred codec:")
                model: ["h264", "h265"]
                currentIndex: controller.draft.preferredCodecs[0] === "h265" ? 1 : 0
                // The unselected codec stays in the list as the fallback offer.
                onActivated: controller.draft.preferredCodecs =
                    currentIndex === 1 ? ["h265", "h264"] : ["h264", "h265"]
            }

            Controls.SpinBox {
                Kirigami.FormData.label: qsTr("Reconnect delay (ms):")
                from: 100
                to: 60000
                stepSize: 100
                value: controller.draft.reconnectInitialMs
                onValueModified: controller.draft.reconnectInitialMs = value
            }

            Controls.SpinBox {
                Kirigami.FormData.label: qsTr("Reconnect max (ms):")
                from: 100
                to: 300000
                stepSize: 500
                value: controller.draft.reconnectMaxMs
                onValueModified: controller.draft.reconnectMaxMs = value
            }
        }

        Kirigami.Separator { Layout.fillWidth: true }

        RowLayout {
            Layout.fillWidth: true

            Kirigami.Heading {
                level: 3
                text: qsTr("Sinks")
                Layout.fillWidth: true
            }

            Controls.Button {
                text: qsTr("Add sink")
                icon.name: "list-add"
                onClicked: addSinkMenu.popup()
            }
        }

        Controls.Menu {
            id: addSinkMenu
            Repeater {
                model: controller.sinkKindNames
                delegate: Controls.MenuItem {
                    required property string modelData
                    text: modelData
                    onTriggered: controller.draft.sinks.addSink(modelData)
                }
            }
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: controller.draft.sinks.count === 0
            icon.name: "list-add"
            text: qsTr("No sinks")
            explanation: qsTr("A stream needs at least one sink to produce output.")
        }

        Repeater {
            model: controller.draft.sinks
            delegate: SinkCard {
                Layout.fillWidth: true
            }
        }
    }
}
