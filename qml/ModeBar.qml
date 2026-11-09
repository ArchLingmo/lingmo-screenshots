/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LingmoUI.CompatibleModule 3.0 as LingmoUI
import Lingmo.System 1.0

// Top of the overlay: photo or video, and what to capture.
LingmoUI.Card {
    id: card

    required property Item editor

    elevation: 2
    padding: LingmoUI.Units.smallSpacing
    implicitWidth: row.implicitWidth + padding * 2
    implicitHeight: row.implicitHeight + padding * 2
    width: implicitWidth
    height: implicitHeight

    readonly property var targets: ["area", "window", "screen"]

    // Keeps the pointer from reaching the capture below.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: LingmoUI.Units.smallSpacing

        LingmoUI.SegmentedControl {
            visible: !editor.scrollMode
            model: [{ text: qsTr("Photo"), iconName: "camera-photo" }, { text: qsTr("Video"), iconName: "media-record" }]
            currentIndex: editor.videoMode ? 1 : 0
            onActivated: (index) => {
                // Recording unavailable in this session (the control keeps "Photo")
                if (index === 1 && !app.canRecord()) {
                    view.message(qsTr("Screen recording is not available in the Wayland session yet."), false)
                    return
                }
                editor.videoMode = index === 1
                editor.setTool("select")
            }
        }

        Label {
            visible: editor.scrollMode
            Layout.leftMargin: LingmoUI.Units.smallSpacing
            text: qsTr("Scrolling capture")
            font.pointSize: LingmoUI.Units.fontBody
            font.bold: true
        }

        LingmoUI.SegmentedControl {
            visible: !editor.scrollMode
            model: [qsTr("Area"), qsTr("Window"), qsTr("Screen")]
            currentIndex: Math.max(0, card.targets.indexOf(editor.target))
            onActivated: (index) => editor.setTarget(card.targets[index])
        }

        LingmoUI.IconButton {
            iconName: "help-about"
            tipText: qsTr("Keyboard shortcuts (F1)")
            checkable: true
            checked: editor.helpVisible
            onClicked: editor.helpVisible = !editor.helpVisible
        }

        LingmoUI.IconButton {
            iconName: "configure"
            tipText: qsTr("Settings")
            onClicked: view.openSettings("settings")
        }

        LingmoUI.IconButton {
            iconName: "window-close"
            tipText: qsTr("Cancel (Esc)")
            onClicked: view.cancel()
        }
    }
}
