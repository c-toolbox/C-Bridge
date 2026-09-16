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

Controls.Dialog {
    id: root

    property var controller

    // Per-stream auto-start rows of the current document: [{id, name, autoStart}].
    property var streamRows: []

    title: qsTr("C-Bridge Preferences")
    modal: true
    width: Math.min(parent ? parent.width - Kirigami.Units.gridUnit * 8 : 560, 560)
    height: Math.min(parent ? parent.height - Kirigami.Units.gridUnit * 8 : 640, 640)
    anchors.centerIn: parent

    function loadFields() {
        configPathField.text = CBridgeSettings.configPath
        autoLoadLastCheck.checked = CBridgeSettings.autoLoadLastConfig
        startOnLoadCheck.checked = CBridgeSettings.startOnLoad
        root.streamRows = root.controller ? root.controller.streamAutoStarts() : []
    }

    function setDefaults() {
        configPathField.text = CBridgeSettings.defaultConfigPathValue
        autoLoadLastCheck.checked = CBridgeSettings.defaultAutoLoadLastConfigValue
        startOnLoadCheck.checked = CBridgeSettings.defaultStartOnLoadValue
        var rows = root.controller ? root.controller.streamAutoStarts() : []
        for (var i = 0; i < rows.length; ++i) {
            rows[i].autoStart = false
        }
        root.streamRows = rows
    }

    onOpened: loadFields()

    FileDialog {
        id: configFileDialog
        title: qsTr("Choose startup configuration")
        nameFilters: [qsTr("C-Bridge configuration (*.json)")]
        onAccepted: configPathField.text = app.urlToPath(selectedFile)
    }

    contentItem: Controls.ScrollView {
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: Math.max(0, parent.width - Kirigami.Units.largeSpacing * 2)
            x: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Heading {
                Layout.fillWidth: true
                level: 2
                text: qsTr("Startup")
            }

            Kirigami.FormLayout {
                Layout.fillWidth: true

                Controls.TextField {
                    id: configPathField
                    Kirigami.FormData.label: qsTr("Configuration file:")
                    Layout.fillWidth: true
                    placeholderText: qsTr("Reload the last opened configuration")
                }
                Controls.Button {
                    text: qsTr("Browse…")
                    icon.name: "document-open"
                    icon.color: Kirigami.Theme.textColor
                    onClicked: configFileDialog.open()
                }

                Controls.CheckBox {
                    id: autoLoadLastCheck
                    Kirigami.FormData.label: qsTr("Fallback:")
                    text: qsTr("Reload the last opened configuration")
                    Controls.ToolTip.text: qsTr("Used when no startup file is set or it cannot be found.")
                    Controls.ToolTip.visible: hovered
                }

                Controls.CheckBox {
                    id: startOnLoadCheck
                    Kirigami.FormData.label: qsTr("Streams:")
                    text: qsTr("Start all enabled streams after the configuration has loaded")
                    Controls.ToolTip.text: qsTr("Individual streams can also be started on load from the list below.")
                    Controls.ToolTip.visible: hovered
                }
            }

            Kirigami.Heading {
                Layout.fillWidth: true
                level: 2
                text: qsTr("Streams")
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Repeater {
                    id: streamRepeater
                    model: root.streamRows

                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            Layout.fillWidth: true
                            text: modelData.name
                            elide: Text.ElideRight
                        }
                        Controls.CheckBox {
                            checked: modelData.autoStart
                            onToggled: modelData.autoStart = checked
                        }
                    }
                }

                Kirigami.PlaceholderMessage {
                    visible: streamRepeater.count === 0
                    Layout.fillWidth: true
                    text: qsTr("No streams in the current configuration.")
                }
            }
        }
    }

    footer: Controls.Pane {
        Kirigami.Theme.colorSet: Kirigami.Theme.Header
        Kirigami.Theme.inherit: false
        implicitHeight: footerLayout.implicitHeight + topPadding + bottomPadding

        RowLayout {
            id: footerLayout

            anchors.fill: parent
            spacing: Kirigami.Units.smallSpacing

            Controls.Button {
                text: qsTr("Load Startup Values")
                icon.name: "document-open-recent"
                icon.color: Kirigami.Theme.textColor
                onClicked: root.loadFields()
            }
            Controls.Button {
                text: qsTr("Load Default Values")
                icon.name: "edit-undo"
                icon.color: Kirigami.Theme.textColor
                onClicked: root.setDefaults()
            }
            Item { Layout.fillWidth: true }
            Controls.Button {
                text: qsTr("Save Startup Values")
                icon.name: "document-save"
                icon.color: Kirigami.Theme.textColor
                onClicked: {
                    if (root.controller) {
                        root.controller.saveStartupSettings(configPathField.text,
                                                            autoLoadLastCheck.checked,
                                                            startOnLoadCheck.checked,
                                                            root.streamRows)
                    }
                }
            }
            Controls.Button {
                text: qsTr("Close")
                icon.name: "dialog-close"
                icon.color: Kirigami.Theme.textColor
                onClicked: root.close()
            }
        }
    }
}

