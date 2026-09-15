/*
 * SPDX-FileCopyrightText:
 * 2026 Erik Sundén
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Required so the nested form Components can reference the "card" id.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.AbstractCard {
    id: card

    required property int index
    required property string kind
    required property bool enabled
    required property string summary

    required property string groupAddress
    required property int port
    required property int ttl
    required property string localAddress
    required property int packetSize
    required property int patPeriodMs
    required property int pcrPeriodMs
    required property string tsUrl

    required property string senderName
    required property int targetWidth
    required property int targetHeight
    required property int fpsNum
    required property int fpsDen
    required property bool ndiAudioEnabled

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Controls.ComboBox {
                model: controller.sinkKindNames
                currentIndex: controller.sinkKindNames.indexOf(card.kind)
                onActivated: controller.draft.sinks.setKind(card.index, currentValue)
            }

            Controls.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                opacity: 0.7
                text: card.summary
            }

            Controls.Switch {
                text: qsTr("Enabled")
                checked: card.enabled
                onToggled: controller.draft.sinks.set(card.index, "enabled", checked)
            }

            Controls.Button {
                icon.name: "list-remove"
                Controls.ToolTip.text: qsTr("Remove this sink")
                Controls.ToolTip.visible: hovered
                onClicked: controller.draft.sinks.removeSink(card.index)
            }
        }

        Loader {
            Layout.fillWidth: true
            sourceComponent: card.kind === "ndi" ? ndiForm : tsForm
        }

        Component {
            id: tsForm

            TsSinkForm {
                sinkRow: card.index
                groupAddress: card.groupAddress
                port: card.port
                ttl: card.ttl
                localAddress: card.localAddress
                packetSize: card.packetSize
                patPeriodMs: card.patPeriodMs
                pcrPeriodMs: card.pcrPeriodMs
                tsUrl: card.tsUrl
            }
        }

        Component {
            id: ndiForm

            NdiSinkForm {
                sinkRow: card.index
                senderName: card.senderName
                targetWidth: card.targetWidth
                targetHeight: card.targetHeight
                fpsNum: card.fpsNum
                fpsDen: card.fpsDen
                audioEnabled: card.ndiAudioEnabled
            }
        }
    }
}
