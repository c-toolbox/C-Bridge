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

    // Prefill with a password entered earlier in this session, if any. The value is kept in
    // memory only and never written to the configuration document.
    Component.onCompleted: passwordField.text = controller.streamPassword(controller.draft.streamId)

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

            Controls.ComboBox {
                Kirigami.FormData.label: qsTr("Source type:")
                model: [qsTr("WHEP (WebRTC)"), qsTr("SRT"), qsTr("YouTube")]
                currentIndex: controller.draft.sourceKind === "srt" ? 1 :
                              controller.draft.sourceKind === "youtube" ? 2 : 0
                onActivated: {
                    const kinds = ["whep", "srt", "youtube"]
                    controller.draft.sourceKind = kinds[currentIndex]
                }
            }

            Controls.TextField {
                Kirigami.FormData.label: qsTr("WHEP URL:")
                visible: controller.draft.sourceKind === "whep"
                enabled: visible
                text: controller.draft.whepUrl
                placeholderText: "http://localhost:8889/mystream/whep"
                onTextEdited: controller.draft.whepUrl = text
            }

            Controls.TextField {
                Kirigami.FormData.label: qsTr("Username:")
                visible: controller.draft.sourceKind === "whep"
                enabled: visible
                text: controller.draft.username
                onTextEdited: controller.draft.username = text
            }

            Controls.TextField {
                id: passwordField
                Kirigami.FormData.label: qsTr("Password:")
                visible: controller.draft.sourceKind === "whep" &&
                         controller.draft.username !== ""
                enabled: visible
                echoMode: Controls.TextField.Password
                placeholderText: qsTr("Session only, or use the Windows Credential Manager")
                onTextChanged: controller.setStreamPassword(controller.draft.streamId, text)
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: controller.draft.sourceKind === "whep" &&
                         controller.draft.username !== "" &&
                         controller.hasStoredCredentialFor(controller.draft.whepUrl, controller.draft.username)
                color: Kirigami.Theme.positiveTextColor
                wrapMode: Text.WordWrap
                text: qsTr("A password for this user is stored in the Windows Credential Manager and will be used automatically.")
            }

            Controls.ComboBox {
                Kirigami.FormData.label: qsTr("SRT mode:")
                visible: controller.draft.sourceKind === "srt"
                enabled: visible
                model: [qsTr("Listener (wait for the encoder)"), qsTr("Caller (dial out)")]
                currentIndex: controller.draft.srtIsCaller ? 1 : 0
                onActivated: controller.draft.srtIsCaller = currentIndex === 1
            }

            Controls.TextField {
                Kirigami.FormData.label: qsTr("Host:")
                visible: controller.draft.sourceKind === "srt" && controller.draft.srtIsCaller
                enabled: visible
                text: controller.draft.srtHost
                placeholderText: "192.168.1.50"
                onTextEdited: controller.draft.srtHost = text
            }

            Controls.SpinBox {
                Kirigami.FormData.label: qsTr("Port:")
                visible: controller.draft.sourceKind === "srt"
                enabled: visible
                from: 1
                to: 65535
                value: controller.draft.srtPort
                onValueModified: controller.draft.srtPort = value
            }

            Controls.TextField {
                Kirigami.FormData.label: qsTr("Stream ID:")
                visible: controller.draft.sourceKind === "srt" && controller.draft.srtIsCaller
                enabled: visible
                placeholderText: "read:myPath (MediaMTX)"
                text: controller.draft.srtStreamId
                onTextEdited: controller.draft.srtStreamId = text
            }

            Controls.TextField {
                Kirigami.FormData.label: qsTr("Passphrase:")
                visible: controller.draft.sourceKind === "srt"
                enabled: visible
                echoMode: Controls.TextField.Password
                placeholderText: qsTr("Optional, must match the encoder")
                text: controller.draft.srtPassphrase
                onTextChanged: controller.draft.srtPassphrase = text
            }

            Controls.SpinBox {
                Kirigami.FormData.label: qsTr("Receive latency (ms):")
                visible: controller.draft.sourceKind === "srt"
                enabled: visible
                from: 0
                to: 60000
                stepSize: 10
                value: controller.draft.srtLatencyMs
                onValueModified: controller.draft.srtLatencyMs = value
            }

            Controls.TextField {
                Kirigami.FormData.label: qsTr("Video URL:")
                visible: controller.draft.sourceKind === "youtube"
                enabled: visible
                text: controller.draft.youtubeUrl
                placeholderText: "https://www.youtube.com/watch?v=…"
                onTextEdited: controller.draft.youtubeUrl = text
            }

            Controls.TextField {
                Kirigami.FormData.label: qsTr("Format selector:")
                visible: controller.draft.sourceKind === "youtube"
                enabled: visible
                placeholderText: qsTr("Empty for yt-dlp's default selection")
                text: controller.draft.formatSelector
                onTextEdited: controller.draft.formatSelector = text
            }

            Controls.TextField {
                Kirigami.FormData.label: qsTr("Extra yt-dlp arguments:")
                visible: controller.draft.sourceKind === "youtube"
                enabled: visible
                placeholderText: "--cookies-from-browser chrome"
                text: controller.draft.extraArgs
                onTextEdited: controller.draft.extraArgs = text
            }

            Controls.SpinBox {
                Kirigami.FormData.label: qsTr("Audio bitrate (kbps):")
                visible: controller.draft.sourceKind === "youtube" && controller.draft.audioEnabled
                enabled: visible
                from: 32
                to: 510
                stepSize: 8
                value: controller.draft.audioBitrateKbps
                onValueModified: controller.draft.audioBitrateKbps = value
            }

            Controls.SpinBox {
                Kirigami.FormData.label: qsTr("Parallel fragments:")
                visible: controller.draft.sourceKind === "youtube"
                enabled: visible
                from: 1
                to: 16
                value: controller.draft.concurrentFragments
                onValueModified: controller.draft.concurrentFragments = value
                Controls.ToolTip.text: qsTr("How many HLS fragments yt-dlp fetches at once (written to the pipe in order). 1 is strictly sequential.")
                Controls.ToolTip.visible: hovered
            }

            Controls.ComboBox {
                Kirigami.FormData.label: qsTr("Direct URL:")
                visible: controller.draft.sourceKind === "youtube"
                enabled: visible
                model: ["auto", "off", "force"]
                currentIndex: ["auto", "off", "force"].indexOf(controller.draft.directUrlMode)
                onActivated: controller.draft.directUrlMode = model[currentIndex]
                Controls.ToolTip.text: qsTr("auto: fetch a muxed H.264+AAC HLS stream directly with FFmpeg (no yt-dlp pipe, instant seek), falling back to the yt-dlp pipe. off: always pipe. force: prefer direct and warn when unavailable.")
                Controls.ToolTip.visible: hovered
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
                visible: controller.draft.sourceKind === "whep"
                enabled: visible
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
