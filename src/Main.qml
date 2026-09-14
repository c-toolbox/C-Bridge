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
        id: addDialog
        title: qsTr("Add stream")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel

        ColumnLayout {
            Controls.TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("Name")
            }
            Controls.TextField {
                id: urlField
                Layout.fillWidth: true
                placeholderText: qsTr("http://localhost:8889/mystream/whep")
            }
        }

        onAccepted: {
            controller.addStream(nameField.text, urlField.text)
            nameField.text = ""
            urlField.text = ""
        }
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
                text: qsTr("Add stream")
                icon.name: "list-add"
                onTriggered: addDialog.open()
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
                            case "Retrying": return Kirigami.Theme.neutralTextColor
                            case "Failed": return Kirigami.Theme.negativeTextColor
                            default: return Kirigami.Theme.disabledTextColor
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Kirigami.Heading {
                            level: 4
                            text: model.name
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            opacity: 0.7
                            text: model.sourceUrl + "  ->  " + model.sinks
                        }
                        Controls.Label {
                            visible: model.lastError !== ""
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            color: Kirigami.Theme.negativeTextColor
                            text: model.lastError
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
                        icon.name: "edit-copy"
                        Controls.ToolTip.text: qsTr("Copy an mpv command for this stream")
                        Controls.ToolTip.visible: hovered
                        onClicked: app.copyToClipboard(controller.mpvCommandFor(model.streamId))
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
        }
    }
}
