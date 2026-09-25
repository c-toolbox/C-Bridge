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

Controls.Dialog {
    id: root

    property var controller

    title: qsTr("MediaMTX Streams")
    modal: true
    width: Math.min(parent ? parent.width - Kirigami.Units.gridUnit * 8 : 960, 960)
    height: Math.min(parent ? parent.height - Kirigami.Units.gridUnit * 8 : 740, 740)
    anchors.centerIn: parent

    property int selectedServerIndex: -1
    property int selectedStreamIndex: -1
    // True when the currently selected fetched path requires read authentication; the
    // "Include credentials in URL" checkbox is forced on and locked for those.
    property bool selectedStreamRequiresAuth: false
    // True when the Windows Credential Manager holds a credential for the currently shown
    // server name + API user ("MediaMTX/<name>/<username>").
    property bool credentialFound: false

    // Reset the dialog state when it becomes visible, not in onOpened: open() flips visible
    // immediately but "opened" only fires after the open animation completes (up to a few
    // hundred ms later). A Fetch Streams / Test Connection click made while the dialog is
    // still animating would otherwise finish before onOpened ran and its clear() would wipe
    // the fresh results, so they flashed briefly and then disappeared. visible also flips
    // back when the dialog closes, hence the guard.
    onVisibleChanged: {
        if (!visible)
            return
        controller.mediaMtxServers.updateServersList()
        root.selectedStreamIndex = -1
        root.selectedStreamRequiresAuth = false
        streamTitleField.text = ""
        // A fetch from a previous dialog session may still be in flight (e.g. an 8 s timeout
        // against an unreachable server); let it populate the list instead of wiping it.
        if (!controller.mediaMtxStreams.refreshInProgress)
            controller.mediaMtxStreams.clear()
        if (controller.mediaMtxServers.numberOfServers > 0) {
            root.selectedServerIndex = 0
            serversList.currentIndex = 0
            root.loadServer(0)
        } else {
            root.selectedServerIndex = -1
            root.clearServerFields()
        }
    }

    // NOTE: Connections handlers must be named on<SignalName>() to be hooked up; a bare
    // function name is just an ordinary method that never fires.
    Connections {
        target: controller.mediaMtxStreams
        function onResponseChanged() {
            if (controller.mediaMtxStreams.lastError !== "") {
                statusLabel.text = qsTr("Error: ") + controller.mediaMtxStreams.lastError
                statusLabel.color = Kirigami.Theme.negativeTextColor
            } else {
                statusLabel.text = controller.mediaMtxStreams.lastSummary
                statusLabel.color = Kirigami.Theme.positiveTextColor
            }
        }
        function onStreamsListChanged() {
            root.selectedStreamIndex = -1
            root.selectedStreamRequiresAuth = false
            streamTitleField.text = ""
            // The WebRTC port/scheme may have been auto-detected from the server config.
            if (root.selectedServerIndex >= 0)
                root.loadServer(root.selectedServerIndex)
        }
    }

    function clearServerFields() {
        serverName.text = ""
        serverHost.text = ""
        serverApiPort.value = 9997
        serverApiScheme.currentIndex = 0
        serverUsername.text = ""
        serverPasswordField.text = ""
        serverManualPassword.checked = false
        root.credentialFound = false
        serverWebRtcPort.value = 8889
        serverWebRtcScheme.currentIndex = 0
        serverAutoDetect.checked = true
        serverEnabled.checked = true
    }

    /// Re-checks the Windows Credential Manager against the current name/user fields and
    /// decides whether a stored or a manually entered password is used.
    function refreshCredentialStatus() {
        root.credentialFound = root.selectedServerIndex >= 0 &&
            controller.mediaMtxServers.hasStoredCredential(serverName.text, serverUsername.text)
        // When falling back to the stored credential, drop any session password so it does
        // not shadow the one from the Credential Manager.
        if (root.credentialFound && !serverManualPassword.checked && root.selectedServerIndex >= 0) {
            serverPasswordField.text = ""
        }
    }

    function loadServer(index) {
        if (index < 0 || index >= controller.mediaMtxServers.numberOfServers)
            return
        var s = controller.mediaMtxServers.serverAt(index)
        serverName.text = s.name
        serverHost.text = s.host
        serverApiPort.value = s.apiPort
        serverApiScheme.currentIndex = s.apiScheme === "https" ? 1 : 0
        serverUsername.text = s.username
        serverWebRtcPort.value = s.webRtcPort
        serverWebRtcScheme.currentIndex = s.webRtcScheme === "https" ? 1 : 0
        serverAutoDetect.checked = s.autoDetectWebRtc
        serverEnabled.checked = s.enabled
    }

    function selectServer(index) {
        if (index < 0 || index >= controller.mediaMtxServers.numberOfServers) {
            root.selectedServerIndex = -1
            root.clearServerFields()
            return
        }
        root.selectedServerIndex = index
        serversList.currentIndex = index
        serverPasswordField.text = "" // the password is session-only, never shown again
        root.loadServer(index)
    }

    function addNewServer() {
        controller.mediaMtxServers.addServer(qsTr("MediaMTX"), "127.0.0.1", 9997, "http", "",
                                             8889, "http", true, true)
        root.selectServer(controller.mediaMtxServers.numberOfServers - 1)
    }

    function updateSelectedServer() {
        if (root.selectedServerIndex < 0)
            return
        controller.mediaMtxServers.updateServer(root.selectedServerIndex, serverName.text,
                                                serverHost.text, serverApiPort.value,
                                                serverApiScheme.currentText, serverUsername.text,
                                                serverWebRtcPort.value, serverWebRtcScheme.currentText,
                                                serverAutoDetect.checked, serverEnabled.checked)
    }

    function removeSelectedServer() {
        if (root.selectedServerIndex < 0)
            return
        controller.mediaMtxServers.removeServer(root.selectedServerIndex)
        root.selectServer(Math.min(root.selectedServerIndex,
                                   controller.mediaMtxServers.numberOfServers - 1))
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // --- Servers -------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true

            Kirigami.Heading {
                level: 2
                text: qsTr("MediaMTX Servers")
                Layout.fillWidth: true
            }

            Controls.Button {
                text: qsTr("Add New")
                icon.name: "list-add"
                onClicked: root.addNewServer()
            }
            Controls.Button {
                text: qsTr("Update")
                icon.name: "document-edit"
                enabled: root.selectedServerIndex >= 0
                onClicked: root.updateSelectedServer()
            }
            Controls.Button {
                text: qsTr("Remove")
                icon.name: "edit-delete"
                enabled: root.selectedServerIndex >= 0
                onClicked: root.removeSelectedServer()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 230
            Layout.minimumHeight: 160
            Layout.maximumHeight: 230
            spacing: Kirigami.Units.largeSpacing

            ColumnLayout {
                Controls.Button {
                    icon.name: "arrow-up"
                    enabled: root.selectedServerIndex > 0
                    onClicked: {
                        controller.mediaMtxServers.moveServer(root.selectedServerIndex,
                                                              root.selectedServerIndex - 1)
                        root.selectServer(root.selectedServerIndex - 1)
                    }
                }
                Controls.Button {
                    icon.name: "arrow-down"
                    enabled: root.selectedServerIndex >= 0 &&
                             root.selectedServerIndex < controller.mediaMtxServers.numberOfServers - 1
                    onClicked: {
                        controller.mediaMtxServers.moveServer(root.selectedServerIndex,
                                                              root.selectedServerIndex + 1)
                        root.selectServer(root.selectedServerIndex + 1)
                    }
                }
            }

            ListView {
                id: serversList
                Layout.preferredWidth: 240
                Layout.fillHeight: true
                clip: true
                model: controller.mediaMtxServers

                delegate: Controls.ItemDelegate {
                    width: serversList.width
                    text: model.name + " — " + model.host
                    highlighted: index === root.selectedServerIndex
                    opacity: model.enabled ? 1.0 : 0.5
                    onClicked: root.selectServer(index)
                }

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    visible: serversList.count === 0
                    text: qsTr("No MediaMTX servers configured")
                }
            }

            Kirigami.FormLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Controls.TextField {
                    id: serverName
                    Kirigami.FormData.label: qsTr("Name:")
                    placeholderText: qsTr("Friendly name for this server")
                }
                Controls.TextField {
                    id: serverHost
                    Kirigami.FormData.label: qsTr("Host:")
                    placeholderText: "192.168.1.50"
                }

                RowLayout {
                    Layout.fillWidth: true
                    Kirigami.FormData.label: qsTr("API endpoint:")

                    Controls.ComboBox {
                        id: serverApiScheme
                        model: ["http", "https"]
                        Layout.preferredWidth: 90
                    }
                    Controls.SpinBox {
                        id: serverApiPort
                        from: 1
                        to: 65535
                        value: 9997
                        Layout.fillWidth: true
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Kirigami.FormData.label: qsTr("WebRTC endpoint:")

                    Controls.ComboBox {
                        id: serverWebRtcScheme
                        model: ["http", "https"]
                        Layout.preferredWidth: 90
                    }
                    Controls.SpinBox {
                        id: serverWebRtcPort
                        from: 1
                        to: 65535
                        value: 8889
                        Layout.fillWidth: true
                    }
                }

                Controls.CheckBox {
                    id: serverAutoDetect
                    Kirigami.FormData.label: qsTr("WebRTC:")
                    text: qsTr("Detect port and scheme from the server configuration")
                    checked: true
                    Controls.ToolTip.text: qsTr("Reads webrtcAddress and webrtcEncryption from the MediaMTX API after each fetch.")
                    Controls.ToolTip.visible: hovered
                }

                Controls.TextField {
                    id: serverUsername
                    Kirigami.FormData.label: qsTr("API user:")
                    placeholderText: qsTr("Optional, for authenticated APIs")
                    onTextChanged: root.refreshCredentialStatus()
                }

                RowLayout {
                    Layout.fillWidth: true
                    Kirigami.FormData.label: qsTr("Password:")

                    Controls.TextField {
                        id: serverPasswordField
                        echoMode: Controls.TextField.Password
                        Layout.fillWidth: true
                        placeholderText: root.credentialFound && !serverManualPassword.checked
                            ? qsTr("Using the Windows Credential Manager")
                            : qsTr("Session only, never stored on disk")
                        enabled: serverManualPassword.checked || !root.credentialFound
                        onTextChanged: {
                            if (root.selectedServerIndex >= 0)
                                controller.mediaMtxServers.setPassword(root.selectedServerIndex, text)
                        }
                    }

                    Controls.CheckBox {
                        id: serverManualPassword
                        text: qsTr("Enter manually")
                        checked: false
                        onToggled: root.refreshCredentialStatus()
                        Controls.ToolTip.text: qsTr("Type a password here instead of using the one from the Windows Credential Manager.")
                        Controls.ToolTip.visible: hovered
                    }
                }

                // Explicitly reports whether a stored credential was found, so the user can
                // always see the outcome of the Windows Credential Manager check.
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.selectedServerIndex >= 0 && serverUsername.text !== ""
                    spacing: Kirigami.Units.smallSpacing

                    // QtQuick.Controls has no standalone icon element in this Qt version, so
                    // use Kirigami's Icon (it resolves theme icons by name).
                    Kirigami.Icon {
                        source: root.credentialFound ? "dialog-ok-true" : "dialog-information"
                        width: Kirigami.Units.iconSizes.Small
                        height: Kirigami.Units.iconSizes.Small
                        color: root.credentialFound ? Kirigami.Theme.positiveTextColor
                                                    : Kirigami.Theme.textColor
                    }

                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: {
                            if (root.credentialFound) {
                                return serverManualPassword.checked
                                    ? qsTr("Credential found in the Windows Credential Manager — the manual password takes precedence.")
                                    : qsTr("Credential found in the Windows Credential Manager — using the stored password.")
                            }
                            return qsTr("No credential found for \"%1\" in the Windows Credential Manager.").arg(
                                "MediaMTX/" + serverName.text + "/" + serverUsername.text)
                        }
                        color: root.credentialFound ? Kirigami.Theme.positiveTextColor
                                                    : Kirigami.Theme.textColor
                    }
                }

                Controls.CheckBox {
                    id: serverEnabled
                    Kirigami.FormData.label: qsTr("Server:")
                    text: qsTr("Enabled")
                    checked: true
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.Button {
                text: qsTr("Test Connection")
                icon.name: "network-connect"
                enabled: root.selectedServerIndex >= 0 && !controller.mediaMtxStreams.refreshInProgress
                onClicked: controller.mediaMtxStreams.testConnection(root.selectedServerIndex)
            }
            Controls.Button {
                text: qsTr("Fetch Streams")
                icon.name: "view-refresh"
                enabled: root.selectedServerIndex >= 0 && !controller.mediaMtxStreams.refreshInProgress
                onClicked: controller.mediaMtxStreams.refresh(root.selectedServerIndex)
            }

            Controls.Label {
                id: statusLabel
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
        }

        // --- Streams -------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true

            Kirigami.Heading {
                level: 2
                text: qsTr("Available Streams")
                Layout.fillWidth: true
            }

            Controls.Label {
                visible: controller.mediaMtxStreams.refreshInProgress
                text: qsTr("Fetching…")
                opacity: 0.6
            }
        }

        ListView {
            id: streamsList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 96
            clip: true
            model: controller.mediaMtxStreams

            delegate: Controls.ItemDelegate {
                width: streamsList.width
                highlighted: index === root.selectedStreamIndex
                opacity: model.online ? 1.0 : 0.6

                contentItem: ColumnLayout {
                    spacing: 2

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: model.online ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            text: (model.serverName !== "" ? model.serverName + "/" : "") + model.name
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Controls.Label {
                            visible: !model.online && model.configuredOnly
                            text: qsTr("configured, not active")
                            opacity: 0.6
                        }

                        Controls.Label {
                            visible: model.requiresAuth
                            text: qsTr("read authentication required")
                            opacity: 0.6
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            visible: model.sourceType !== ""
                            text: qsTr("Source: %1").arg(model.sourceType)
                            opacity: 0.6
                        }
                        Controls.Label {
                            visible: model.tracks !== ""
                            Layout.fillWidth: true
                            text: model.tracks
                            elide: Text.ElideRight
                            opacity: 0.6
                        }
                        Controls.Label {
                            visible: model.readers > 0
                            text: qsTr("%1 reader(s)").arg(model.readers)
                            opacity: 0.6
                        }
                    }

                    Controls.Label {
                        Layout.fillWidth: true
                        text: model.whepUrl
                        color: Kirigami.Theme.disabledTextColor
                        elide: Text.ElideMiddle
                    }
                }

                onClicked: {
                    root.selectedStreamIndex = index
                    streamTitleField.text = model.serverName !== "" ? model.serverName + "/" + model.name : model.name
                    // A path with read authentication cannot be fetched without credentials, so the
                    // checkbox is forced on for those instead of offering a choice that would create
                    // an entry that never connects.
                    root.selectedStreamRequiresAuth = model.requiresAuth
                    includeCredentialsCheckBox.checked = model.requiresAuth
                    if (model.requiresAuth && controller.mediaMtxStreams.currentServerIndex >= 0 &&
                        !controller.mediaMtxServers.hasUsablePassword(controller.mediaMtxStreams.currentServerIndex)) {
                        statusLabel.text = qsTr("This path requires read authentication, but no password is available for its server.")
                        statusLabel.color = Kirigami.Theme.negativeTextColor
                    }
                }
            }

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                visible: streamsList.count === 0 && !controller.mediaMtxStreams.refreshInProgress
                icon.name: "network-connect"
                text: qsTr("No streams fetched yet")
                explanation: qsTr("Select a server and press Fetch Streams.")
            }

            Controls.ScrollBar.vertical: Controls.ScrollBar {}
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.TextField {
                id: streamTitleField
                Layout.fillWidth: true
                placeholderText: qsTr("Name for the new C-Bridge stream entry")
                enabled: root.selectedStreamIndex >= 0
            }
            Controls.CheckBox {
                id: includeCredentialsCheckBox
                text: qsTr("Include credentials in URL")
                checked: false
                // Locked on for paths that require read authentication; without the credentials
                // they cannot be fetched at all.
                enabled: root.selectedStreamIndex >= 0 && !root.selectedStreamRequiresAuth
                Controls.ToolTip.text: qsTr("Embeds user:password in the WHEP URL. C-Bridge sends them as an HTTP Basic header, but they are written to the configuration file in clear text. Enabled automatically for paths that require read authentication.")
                Controls.ToolTip.visible: hovered
            }
            Controls.Button {
                text: qsTr("Add to Configuration")
                icon.name: "list-add"
                enabled: root.selectedStreamIndex >= 0
                onClicked: {
                    if (controller.addMediaMtxStream(root.selectedStreamIndex, streamTitleField.text,
                                                     includeCredentialsCheckBox.checked)) {
                        statusLabel.text = qsTr("Added to configuration")
                        statusLabel.color = Kirigami.Theme.positiveTextColor
                    } else {
                        statusLabel.text = controller.statusMessage
                        statusLabel.color = Kirigami.Theme.negativeTextColor
                    }
                }
            }
        }
    }
}