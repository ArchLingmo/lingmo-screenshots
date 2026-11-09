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

// New capture: what to capture and when.
ColumnLayout {
    id: page
    spacing: 0

    readonly property var modes: [
        { mode: "area", icon: "select-rectangular", title: qsTr("Area"), text: qsTr("Drag over what you want, or click a highlighted part") },
        { mode: "window", icon: "window", title: qsTr("Window"), text: qsTr("Click a window to capture it") },
        { mode: "screen", icon: "view-fullscreen", title: qsTr("Screen"), text: qsTr("The screen under the pointer") },
        { mode: "full", icon: "video-display", title: qsTr("All screens"), text: qsTr("Every monitor in one image") },
        { mode: "scroll", icon: "go-down", title: qsTr("Scrolling"), text: qsTr("A long page, scrolled and joined for you") },
        { mode: "record", icon: "media-record", title: qsTr("Record screen"), text: qsTr("Video or GIF of an area or a screen") }
    ]

    LingmoUI.PageHeader {
        title: qsTr("New capture")
        large: false
        leftPadding: LingmoUI.Units.largeSpacing * 2
        rightPadding: LingmoUI.Units.largeSpacing * 2
        Layout.fillWidth: true
    }

    Flickable {
        id: flick
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentHeight: content.implicitHeight + LingmoUI.Units.largeSpacing * 2
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        LingmoUI.WheelHandler { target: flick }

        ColumnLayout {
            id: content
            x: LingmoUI.Units.largeSpacing * 2
            y: LingmoUI.Units.largeSpacing
            width: flick.width - LingmoUI.Units.largeSpacing * 4
            spacing: LingmoUI.Units.largeSpacing

            LingmoUI.Banner {
                Layout.fillWidth: true
                visible: app.recording
                type: LingmoUI.Banner.Info
                title: qsTr("Recording the screen")
                text: qsTr("Stop it here, with the indicator on the screen or with lingmo-screenshot --stop-recording.")
                closable: false

                LingmoUI.ActionButton {
                    text: qsTr("Stop")
                    destructive: true
                    iconName: "media-playback-stop"
                    onClicked: app.stopRecording()
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: width > 640 ? 3 : 2
                columnSpacing: LingmoUI.Units.largeSpacing
                rowSpacing: LingmoUI.Units.largeSpacing

                Repeater {
                    model: page.modes

                    delegate: LingmoUI.Card {
                        id: tile
                        required property var modelData

                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        implicitHeight: tileColumn.implicitHeight + LingmoUI.Units.largeSpacing * 3
                        clickable: true
                        liftOnHover: true
                        // Wayland: the scrolling capture is X11 only for now; recording goes
                        // through KWin's screencast when GStreamer's PipeWire plugin is there
                        readonly property bool x11Only: modelData.mode === "scroll" ? !Session.supports("x11")
                                                      : modelData.mode === "record" && !app.canRecord()
                        enabled: (modelData.mode !== "record" || !app.recording) && !x11Only
                        opacity: enabled ? 1 : 0.5

                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.title

                        onClicked: app.captureFromLauncher(modelData.mode, delayBox.value)

                        ColumnLayout {
                            id: tileColumn
                            anchors.fill: parent
                            anchors.margins: LingmoUI.Units.largeSpacing * 1.5
                            spacing: LingmoUI.Units.smallSpacing

                            LingmoUI.IconItem {
                                source: tile.modelData.icon
                                Layout.preferredWidth: LingmoUI.Units.iconSizes.medium
                                Layout.preferredHeight: LingmoUI.Units.iconSizes.medium
                                Layout.bottomMargin: LingmoUI.Units.smallSpacing
                            }

                            Label {
                                text: tile.modelData.title
                                font.pointSize: LingmoUI.Units.fontSubheading
                                font.bold: true
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }

                            Label {
                                text: tile.modelData.text
                                color: LingmoUI.Theme.secondaryTextColor
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            SessionHint {
                                session: "x11"
                                visible: tile.x11Only
                                text: qsTr("Not available in the Wayland session yet")
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }

            LingmoUI.Card {
                Layout.fillWidth: true
                implicitHeight: delayRow.implicitHeight + LingmoUI.Units.largeSpacing * 2

                RowLayout {
                    id: delayRow
                    anchors.fill: parent
                    anchors.leftMargin: LingmoUI.Units.largeSpacing * 1.5
                    anchors.rightMargin: LingmoUI.Units.largeSpacing * 1.5
                    spacing: LingmoUI.Units.largeSpacing

                    LingmoUI.IconItem {
                        source: "timer-symbolic"
                        Layout.preferredWidth: LingmoUI.Units.iconSizes.smallMedium
                        Layout.preferredHeight: LingmoUI.Units.iconSizes.smallMedium
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            text: qsTr("Delay")
                            font.pointSize: LingmoUI.Units.fontBody
                        }
                        Label {
                            text: qsTr("Time to open a menu or a tooltip before the capture.")
                            color: LingmoUI.Theme.secondaryTextColor
                            font.pointSize: LingmoUI.Units.fontCaption
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }

                    SpinBox {
                        id: delayBox
                        from: 0
                        to: 60
                        value: 0
                        editable: true
                        textFromValue: function(value) { return qsTr("%n s", "", value) }
                        valueFromText: function(text) { return parseInt(text) || 0 }
                    }
                }
            }

            LingmoUI.Card {
                Layout.fillWidth: true
                implicitHeight: tipsColumn.implicitHeight + LingmoUI.Units.largeSpacing * 2

                ColumnLayout {
                    id: tipsColumn
                    anchors.fill: parent
                    anchors.leftMargin: LingmoUI.Units.largeSpacing * 1.5
                    anchors.rightMargin: LingmoUI.Units.largeSpacing * 1.5
                    anchors.topMargin: LingmoUI.Units.largeSpacing
                    anchors.bottomMargin: LingmoUI.Units.largeSpacing
                    spacing: LingmoUI.Units.smallSpacing

                    Label {
                        text: qsTr("Shortcuts")
                        font.pointSize: LingmoUI.Units.fontBody
                        font.bold: true
                    }
                    Label {
                        text: qsTr("Ctrl+Alt+A starts a capture anywhere. From a terminal: lingmo-screenshot gui, full, screen, window, record — see lingmo-screenshot --help.")
                        color: LingmoUI.Theme.secondaryTextColor
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
}
