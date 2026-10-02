/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root

    title: qsTr("C-Bridge %1 — %2%3")
        .arg(app.version)
        .arg(controller.configName)
        .arg(controller.dirty ? " *" : "")
    visible: true

    minimumWidth: 1120
    minimumHeight: 720
    width: 1440
    height: 900

    // The C++ context property "controller" is shadowed by the dialogs' own
    // "property var controller": a binding like "controller: controller" inside an inline
    // dialog instance resolves to the dialog's own (initially undefined) property, so the
    // real controller must be handed over through this window-level alias instead.
    property var bridgeController: controller

    Kirigami.Theme.inherit: false
    Kirigami.Theme.colorSet: Kirigami.Theme.Window

    FileDialog {
        id: openDialog
        title: qsTr("Open configuration")
        nameFilters: [qsTr("C-Bridge configuration (*.json)")]
        onAccepted: controller.loadConfig(app.urlToPath(selectedFile))
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Save configuration")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("C-Bridge configuration (*.json)")]
        onAccepted: controller.saveConfig(app.urlToPath(selectedFile))
    }

    Kirigami.PromptDialog {
        id: removeDialog
        title: qsTr("Remove stream")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel

        property string streamId: ""
        property string streamName: ""

        subtitle: qsTr("Remove \"%1\" from this configuration?").arg(streamName)
        onAccepted: controller.removeStream(streamId)
    }

    SettingsDialog {
        id: settingsDialog
        controller: root.bridgeController
    }

    MediaMTXDialog {
        id: mediaMtxDialog
        controller: root.bridgeController
    }

    Component {
        id: editorPage
        StreamEditorPage {}
    }

    Component {
        id: previewWindow
        PreviewWindow { controller: root.bridgeController }
    }

    function openEditor(streamId) {
        controller.beginEditStream(streamId)
        pageStack.push(editorPage)
    }

    /// mm:ss (h:mm:ss past an hour) for the transport labels.
    function formatTime(seconds) {
        if (!seconds || seconds <= 0)
            return "0:00"
        const total = Math.floor(seconds)
        const h = Math.floor(total / 3600)
        const m = Math.floor((total % 3600) / 60)
        const s = total % 60
        const mm = h > 0 ? String(m).padStart(2, "0") : String(m)
        return (h > 0 ? h + ":" : "") + mm + ":" + String(s).padStart(2, "0")
    }

    /// Opens (or reuses) the built-in viewer for a stream. One window per stream, kept in a
    /// map on the window so re-clicking Preview raises the existing viewer instead of stacking
    /// another copy. The window detaches itself when the stream stops.
    property var previewWindows: ({})

    function openPreview(streamId, streamName) {
        let win = root.previewWindows[streamId]
        if (win) {
            win.streamName = streamName
            win.show()
            win.raise()
            win.requestActivate()
            return
        }
        win = previewWindow.createObject(root)
        if (!win)
            return
        win.streamId = streamId
        win.streamName = streamName
        // Window has no "closed" signal; "closing" (with the CloseEvent) is the one that
        // fires as the window goes away. Connecting a non-existent signal would throw and
        // abort this function before show() below ever ran.
        win.closing.connect(function () {
            delete root.previewWindows[streamId]
        })
        root.previewWindows[streamId] = win
        win.show()
    }

    pageStack.initialPage: Kirigami.ScrollablePage {
        title: qsTr("Streams")

        actions: [
            Kirigami.Action {
                text: qsTr("Open")
                icon.name: "document-open"
                onTriggered: openDialog.open()
            },
            Kirigami.Action {
                text: qsTr("Save")
                icon.name: "document-save"
                onTriggered: saveDialog.open()
            },
            Kirigami.Action {
                text: qsTr("Settings")
                icon.name: "configure"
                onTriggered: settingsDialog.open()
            },
            Kirigami.Action {
                text: qsTr("Add stream")
                icon.name: "list-add"
                onTriggered: root.openEditor("")
            },
            Kirigami.Action {
                text: qsTr("MediaMTX streams…")
                icon.name: "network-connect"
                onTriggered: mediaMtxDialog.open()
            },
            Kirigami.Action {
                text: controller.running ? qsTr("Stop all") : qsTr("Start all")
                icon.name: controller.running ? "media-playback-stop" : "media-playback-start"
                onTriggered: controller.running ? controller.stopAll() : controller.startAll()
            }
        ]

        ListView {
            id: streamList
            model: controller.streams
            spacing: Kirigami.Units.smallSpacing

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                visible: streamList.count === 0
                icon.name: "network-connect"
                text: qsTr("No streams")
                explanation: qsTr("Add a WHEP stream to bridge it to multicast or NDI.")
            }

            delegate: Kirigami.AbstractCard {
                width: ListView.view.width

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: Kirigami.Units.gridUnit * 0.7
                        implicitHeight: implicitWidth
                        radius: width / 2
                        color: {
                            switch (model.state) {
                            case "Running": return Kirigami.Theme.positiveTextColor
                            case "Connecting":
                            case "Retrying":
                            case "Paused": return Kirigami.Theme.neutralTextColor
                            case "Failed": return Kirigami.Theme.negativeTextColor
                            default: return Kirigami.Theme.disabledTextColor
                            }
                        }
                    }

                    ColumnLayout {
                        id: infoColumn
                        Layout.fillWidth: true
                        spacing: 0

                        // Captured here because inside the Repeater "model" is the sink list.
                        readonly property var sinkRows: model.sinkStats

                        Kirigami.Heading {
                            level: 4
                            text: model.name
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            opacity: 0.7
                            text: model.sourceUrl
                        }

                        Repeater {
                            model: infoColumn.sinkRows
                            delegate: RowLayout {
                                id: sinkRow
                                required property var modelData
                                spacing: Kirigami.Units.smallSpacing

                                Rectangle {
                                    implicitWidth: Kirigami.Units.gridUnit * 0.5
                                    implicitHeight: implicitWidth
                                    radius: width / 2
                                    color: sinkRow.modelData.open
                                        ? Kirigami.Theme.positiveTextColor
                                        : Kirigami.Theme.disabledTextColor
                                }
                                Controls.Label {
                                    opacity: 0.7
                                    text: sinkRow.modelData.description + "  " + sinkRow.modelData.mbps + " Mbps"
                                }
                            }
                        }

                        Controls.Label {
                            visible: model.lastError !== ""
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            color: Kirigami.Theme.negativeTextColor
                            text: model.lastError
                        }

                        // The source's last recovery event (stall resume, direct-url switch,
                        // pipe fallback) — diagnostic context that must not be confused with
                        // the red error line above.
                        Controls.Label {
                            visible: model.lastEvent !== undefined && model.lastEvent !== ""
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            opacity: 0.6
                            text: model.lastEvent
                        }

                        // YouTube VOD transport: pause/resume and a seek slider. Shown only
                        // when the running source reports it can control its timeline; live
                        // streams hide the slider (no seekable duration). Seek commits on
                        // release because each seek respawns the yt-dlp child.
                        RowLayout {
                            id: transportRow
                            Layout.fillWidth: true
                            visible: model.playbackControllable === true
                            spacing: Kirigami.Units.smallSpacing

                            readonly property real duration: model.durationSeconds
                            readonly property real position: model.positionSeconds

                            // The playhead is pushed rather than bound to the slider: a drag
                            // would destroy the binding permanently, and the delegate lives on.
                            onPositionChanged:
                                if (!cardSeekSlider.pressed)
                                    cardSeekSlider.value = position

                            Controls.Button {
                                icon.name: model.state === "Paused"
                                    ? "media-playback-start" : "media-playback-pause"
                                Controls.ToolTip.text: model.state === "Paused"
                                    ? qsTr("Resume") : qsTr("Pause")
                                Controls.ToolTip.visible: hovered
                                onClicked: {
                                    if (model.state === "Paused")
                                        controller.resumeStream(model.streamId)
                                    else
                                        controller.pauseStream(model.streamId)
                                }
                            }

                            Controls.Label {
                                text: root.formatTime(transportRow.position)
                                opacity: 0.7
                            }

                            Controls.Slider {
                                id: cardSeekSlider
                                Layout.fillWidth: true
                                visible: !model.live && transportRow.duration > 0
                                from: 0
                                to: Math.max(transportRow.duration, 1)
                                enabled: !model.live
                                onPressedChanged: {
                                    if (!pressed)
                                        controller.seekStream(model.streamId, value)
                                }
                            }

                            Controls.Label {
                                text: model.live ? qsTr("LIVE") : root.formatTime(transportRow.duration)
                                opacity: model.live ? 1.0 : 0.7
                                color: model.live ? Kirigami.Theme.negativeTextColor
                                                   : Kirigami.Theme.textColor
                            }
                        }
                    }

                    GridLayout {
                        columns: 2
                        columnSpacing: Kirigami.Units.largeSpacing
                        rowSpacing: 0

                        Controls.Label { text: qsTr("State"); opacity: 0.6 }
                        Controls.Label { text: model.state }
                        Controls.Label { text: qsTr("Video"); opacity: 0.6 }
                        Controls.Label { text: model.resolution + " " + model.codec }
                        Controls.Label { text: qsTr("Rate"); opacity: 0.6 }
                        Controls.Label { text: model.mbps + " Mbps" }
                        Controls.Label { text: qsTr("Queue / dropped"); opacity: 0.6 }
                        Controls.Label { text: model.queueDepth + " / " + model.dropped }
                    }

                    Controls.Button {
                        text: (model.state === "Idle" || model.state === "Failed")
                            ? qsTr("Start") : qsTr("Stop")
                        onClicked: {
                            if (model.state === "Idle" || model.state === "Failed")
                                controller.startStream(model.streamId)
                            else
                                controller.stopStream(model.streamId)
                        }
                    }

                    Controls.Button {
                        icon.name: "view-preview"
                        Controls.ToolTip.text: qsTr("Preview this stream")
                        Controls.ToolTip.visible: hovered
                        onClicked: root.openPreview(model.streamId, model.name)
                    }

                    Controls.Button {
                        icon.name: "edit-copy"
                        Controls.ToolTip.text: qsTr("Copy the multicast address for this stream")
                        Controls.ToolTip.visible: hovered
                        onClicked: app.copyToClipboard(controller.tsAddressFor(model.streamId))
                    }

                    Controls.Button {
                        icon.name: "document-edit"
                        Controls.ToolTip.text: qsTr("Edit this stream")
                        Controls.ToolTip.visible: hovered
                        onClicked: root.openEditor(model.streamId)
                    }

                    Controls.Button {
                        icon.name: "edit-delete"
                        Controls.ToolTip.text: qsTr("Remove this stream")
                        Controls.ToolTip.visible: hovered
                        onClicked: {
                            removeDialog.streamId = model.streamId
                            removeDialog.streamName = model.name
                            removeDialog.open()
                        }
                    }
                }
            }
        }
    }

    footer: Controls.ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Kirigami.Units.largeSpacing
            anchors.rightMargin: Kirigami.Units.largeSpacing

            Controls.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: controller.statusMessage
            }

            Controls.Label {
                text: qsTr("Total in: %1 Mbps").arg(controller.aggregateMbps)
            }

            Kirigami.Separator {
                Layout.fillHeight: true
                Layout.margins: Kirigami.Units.smallSpacing
            }

            Controls.Label {
                text: qsTr("Total out: %1 Mbps").arg(controller.aggregateOutMbps)
            }
        }
    }
}
