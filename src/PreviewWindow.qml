/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtMultimedia

import org.kde.kirigami as Kirigami

/*
 * The built-in viewer for one stream.
 *
 * The video comes from the pipeline's StreamPreview, which decodes the stream's own bitstream
 * on the pipeline's worker thread and publishes frames through a QVideoSink — the same object
 * the VideoOutput below consumes. Nothing is decoded at all while this window is closed: the
 * preview only produces frames while its `active` property is true, which this window owns.
 *
 * A separate top-level Window rather than a dialog, so the viewer can be dragged onto a second
 * display and resized next to the main window while the bridge keeps running.
 *
 * For a YouTube VOD stream the footer also carries the transport controls (pause/resume and a
 * seek slider). Those drive the source through the controller; a seek respawns the yt-dlp
 * child, so the slider commits on release rather than on every move.
 */
Window {
    id: root

    property var controller
    // Not a required property: the window is created through Component.createObject(), which
    // would fail (and silently return null) unless every required property were passed in the
    // creation context. streamId is assigned right after creation instead.
    property string streamId: ""
    property string streamName: ""

    /// The pipeline's preview object, fetched while the window is visible. Null whenever the
    /// stream is not running, which the placeholder message covers.
    property var preview: null
    property bool controllable: false
    property bool live: false
    property real positionSeconds: 0
    property real durationSeconds: 0
    property string streamState: "Idle"

    title: streamName.length > 0
        ? qsTr("C-Bridge Preview — %1").arg(streamName)
        : qsTr("C-Bridge Preview")
    width: Math.min(960, Screen.width - Kirigami.Units.gridUnit * 4)
    height: Math.min(620, Screen.height - Kirigami.Units.gridUnit * 4)
    minimumWidth: 360
    minimumHeight: 240

    Kirigami.Theme.inherit: false
    Kirigami.Theme.colorSet: Kirigami.Theme.Window
    color: Kirigami.Theme.backgroundColor

    // Attach the viewer on show rather than after the window finishes mapping: waiting would
    // delay the first frame for no benefit. visible also flips back on close, hence the guard.
    onVisibleChanged: {
        if (visible) {
            root.refresh()
            root.attachSink()
            pollTimer.restart()
        } else {
            if (root.preview)
                root.preview.detach()
            root.preview = null
            pollTimer.stop()
        }
    }

    /// Hands the VideoOutput's own sink to the preview, which pushes decoded frames into it.
    /// VideoOutput owns its QVideoSink (the property is read-only), so the push direction is
    /// preview -> sink, exactly like a media player feeding its output.
    function attachSink() {
        if (root.preview)
            root.preview.attachTo(videoOutput.videoSink)
    }

    // The preview object dies with its pipeline, so a window left open on a stopped stream
    // must close rather than keep showing a frozen last frame.
    Connections {
        target: root.controller
        function onPreviewInvalidated(streamId) {
            if (streamId === root.streamId)
                root.close()
        }
    }

    function formatTime(seconds) {
        if (seconds <= 0)
            return "0:00"
        const total = Math.floor(seconds)
        const h = Math.floor(total / 3600)
        const m = Math.floor((total % 3600) / 60)
        const s = total % 60
        const mm = h > 0 ? String(m).padStart(2, "0") : String(m)
        return (h > 0 ? h + ":" : "") + mm + ":" + String(s).padStart(2, "0")
    }

    /// Re-resolves the preview object and the transport state from the controller. Called on
    /// show and on the 1 Hz poll, so a stream started after the window opened still appears.
    function refresh() {
        const p = controller.previewFor(root.streamId)
        if (p !== root.preview) {
            if (root.preview)
                root.preview.detach()
            root.preview = p
            if (root.preview && root.visible)
                root.preview.attachTo(videoOutput.videoSink)
        }
        root.controllable = controller.isPlaybackControllable(root.streamId)
        root.live = controller.isLiveStream(root.streamId)
        root.streamState = controller.streamState(root.streamId)
        root.durationSeconds = controller.streamDurationSeconds(root.streamId)
        // Only move the playhead while the user is not dragging the slider.
        if (seekSlider && !seekSlider.pressed) {
            root.positionSeconds = controller.streamPositionSeconds(root.streamId)
            seekSlider.value = root.positionSeconds
        }
    }

    // Position and duration ride the engine's 1 Hz stats poll, so the window polls at the same
    // rate rather than receiving a signal per frame.
    Timer {
        id: pollTimer
        interval: 1000
        repeat: true
        running: false
        onTriggered: root.refresh()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Rectangle {
                anchors.fill: parent
                color: "black"
            }

            VideoOutput {
                id: videoOutput
                anchors.fill: parent
                visible: root.preview !== null && root.preview.hasFrame
                fillMode: VideoOutput.PreserveAspectFit
            }

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                visible: !videoOutput.visible
                icon.name: "video-x-generic"
                text: root.streamState === "Idle"
                    ? qsTr("Stream not running")
                    : qsTr("Waiting for video…")
                explanation: root.streamState === "Idle"
                    ? qsTr("Start this stream to see it here.")
                    : qsTr("The viewer shows the decoded video of this stream, once a keyframe arrives. If the stream is paused, resume it first.")
            }
        }

        // --- Transport -------------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            visible: root.controllable
            spacing: Kirigami.Units.smallSpacing

            Controls.Button {
                icon.name: root.streamState === "Paused"
                    ? "media-playback-start" : "media-playback-pause"
                Controls.ToolTip.text: root.streamState === "Paused"
                    ? qsTr("Resume") : qsTr("Pause")
                Controls.ToolTip.visible: hovered
                enabled: root.streamState === "Running" || root.streamState === "Paused"
                onClicked: {
                    if (root.streamState === "Paused")
                        controller.resumeStream(root.streamId)
                    else
                        controller.pauseStream(root.streamId)
                    root.streamState = controller.streamState(root.streamId)
                }
            }

            Controls.Label {
                text: root.formatTime(root.positionSeconds)
                opacity: 0.7
            }

            Controls.Slider {
                id: seekSlider
                Layout.fillWidth: true
                visible: !root.live && root.durationSeconds > 0
                from: 0
                to: Math.max(root.durationSeconds, 1)
                enabled: !root.live

                // The playhead is pushed imperatively from refresh() rather than bound: a
                // binding would be destroyed by the first drag and never come back.
                onPressedChanged: {
                    if (!pressed) {
                        controller.seekStream(root.streamId, value)
                        root.positionSeconds = value
                    }
                }
            }

            Controls.Label {
                text: root.live ? qsTr("LIVE") : root.formatTime(root.durationSeconds)
                opacity: root.live ? 1.0 : 0.7
                color: root.live ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
            }

            Controls.Button {
                icon.name: root.preview && root.preview.muted
                    ? "player-volume-muted" : "player-volume"
                Controls.ToolTip.text: qsTr("Mute this viewer")
                Controls.ToolTip.visible: hovered
                enabled: root.preview !== null
                onClicked: {
                    if (root.preview)
                        root.preview.muted = !root.preview.muted
                }
            }
        }
    }
}
