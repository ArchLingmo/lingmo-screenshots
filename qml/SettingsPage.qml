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

ColumnLayout {
    id: page
    spacing: 0

    component SectionCard: LingmoUI.Card {
        id: sectionCard
        property string heading
        default property alias rows: sectionColumn.data
        Layout.fillWidth: true
        implicitHeight: sectionColumn.implicitHeight + LingmoUI.Units.largeSpacing * 2

        ColumnLayout {
            id: sectionColumn
            anchors.fill: parent
            anchors.leftMargin: sectionCard.padding
            anchors.rightMargin: sectionCard.padding
            anchors.topMargin: LingmoUI.Units.largeSpacing
            anchors.bottomMargin: LingmoUI.Units.largeSpacing
            spacing: LingmoUI.Units.smallSpacing

            Label {
                text: sectionCard.heading
                font.pointSize: LingmoUI.Units.fontSubheading
                font.bold: true
                Layout.bottomMargin: LingmoUI.Units.smallSpacing
            }
        }
    }

    LingmoUI.PageHeader {
        title: qsTr("Settings")
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

            // ------------------------------------------------ saving
            SectionCard {
                heading: qsTr("Saving")

                SettingRow {
                    title: qsTr("Folder")
                    subtitle: config.saveFolder

                    LingmoUI.IconButton {
                        iconName: "folder-open"
                        tipText: qsTr("Open the folder")
                        onClicked: config.openSaveFolder()
                    }
                    LingmoUI.ActionButton {
                        text: qsTr("Change…")
                        onClicked: config.chooseSaveFolder()
                    }
                }

                SettingRow {
                    title: qsTr("File name")
                    subtitle: qsTr("Example: %1").arg(config.previewFileName(patternField.text))

                    TextField {
                        id: patternField
                        Layout.preferredWidth: 300
                        text: config.fileNamePattern
                        selectByMouse: true
                        onEditingFinished: config.fileNamePattern = text
                    }
                    LingmoUI.IconButton {
                        iconName: "edit-undo"
                        tipText: qsTr("Default name")
                        enabled: patternField.text !== config.defaultFileNamePattern
                        onClicked: {
                            patternField.text = config.defaultFileNamePattern
                            config.fileNamePattern = patternField.text
                        }
                    }
                }

                Label {
                    text: qsTr("%Y year, %m month, %d day, %H hour, %M minute, %S second, %w × %h size, %t window title")
                    color: LingmoUI.Theme.secondaryTextColor
                    font.pointSize: LingmoUI.Units.fontCaption
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                SettingRow {
                    title: qsTr("Format")
                    ComboBox {
                        model: ["PNG", "JPG", "WebP"]
                        currentIndex: ["png", "jpg", "webp"].indexOf(config.imageFormat)
                        onActivated: (index) => config.imageFormat = ["png", "jpg", "webp"][index]
                    }
                }

                SettingRow {
                    title: qsTr("Copy to the clipboard when saving")
                    Switch {
                        checked: config.copyAfterSave
                        onToggled: config.copyAfterSave = checked
                    }
                }
            }

            // ------------------------------------------------ capture
            SectionCard {
                heading: qsTr("Capture")

                SettingRow {
                    title: qsTr("Shutter sound")
                    Switch { checked: config.playSound; onToggled: config.playSound = checked }
                }
                SettingRow {
                    title: qsTr("Show the shortcuts when capturing")
                    Switch { checked: config.showHelp; onToggled: config.showHelp = checked }
                }
                SettingRow {
                    title: qsTr("Magnifier")
                    subtitle: qsTr("Enlarged pixels, position and colour next to the pointer.")
                    Switch { checked: config.showMagnifier; onToggled: config.showMagnifier = checked }
                }
                SettingRow {
                    title: qsTr("Smart selection")
                    subtitle: qsTr("Highlights windows and parts of them under the pointer; click to select one, scroll for a bigger one.")
                    Switch { checked: config.smartSelection; onToggled: config.smartSelection = checked }
                }
            }

            // ------------------------------------------------ text recognition
            SectionCard {
                heading: qsTr("Text recognition")

                Label {
                    text: qsTr("Copy text and Quick redact read the selection with tesseract, on this computer. Nothing is sent anywhere.")
                    color: LingmoUI.Theme.secondaryTextColor
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                LingmoUI.Banner {
                    Layout.fillWidth: true
                    closable: false
                    type: ocr.missingPackages.length === 0 ? LingmoUI.Banner.Success : LingmoUI.Banner.Warning
                    title: ocr.missingPackages.length === 0 ? qsTr("Ready") : qsTr("Not installed")
                    text: ocr.missingPackages.length === 0
                          ? qsTr("Languages: %1").arg(ocr.languages.join(", "))
                          : qsTr("Missing packages: %1").arg(ocr.missingPackages.join(" "))

                    LingmoUI.ActionButton {
                        visible: ocr.missingPackages.length > 0
                        text: ocr.installing ? qsTr("Installing…") : qsTr("Install")
                        primary: true
                        busy: ocr.installing
                        onClicked: ocr.install()
                    }
                }

                SettingRow {
                    title: qsTr("Reading languages")
                    subtitle: qsTr("Automatic: the script of the text is detected and its installed pack is used. Add languages (Hebrew, Arabic, Chinese…) in Settings.")
                    LingmoUI.ActionButton {
                        text: qsTr("Manage…")
                        onClicked: ocr.openLanguageSettings()
                    }
                }

                SettingRow {
                    title: qsTr("Preferred Latin languages")
                    subtitle: qsTr("Advanced. Only used when the script can't be detected; leave empty for all installed Latin packs. Tesseract codes joined with +, for example por+eng.")
                    TextField {
                        Layout.preferredWidth: 180
                        text: config.ocrLanguages
                        placeholderText: qsTr("Automatic")
                        selectByMouse: true
                        onEditingFinished: config.ocrLanguages = text
                    }
                }

                Connections {
                    target: ocr
                    function onInstallFinished(ok, message) {
                        if (!ok && message.length > 0)
                            rootWindow.showPassiveNotification(message, "long")
                    }
                }
            }

            // ------------------------------------------------ presentation
            SectionCard {
                heading: qsTr("Presentation")

                Label {
                    text: qsTr("With the presentation button, the capture goes on a background with rounded corners and a shadow.")
                    color: LingmoUI.Theme.secondaryTextColor
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: LingmoUI.Units.largeSpacing * 2

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: LingmoUI.Units.smallSpacing

                        SettingRow {
                            title: qsTr("Background")
                            Repeater {
                                model: config.beautifyBackgrounds
                                delegate: AbstractButton {
                                    id: bgButton
                                    required property var modelData
                                    required property int index
                                    implicitWidth: 26
                                    implicitHeight: 26
                                    hoverEnabled: true
                                    ToolTip.visible: false
                                    background: Rectangle {
                                        radius: width / 2
                                        border.width: config.beautifyBackground === bgButton.index ? 3 : 1
                                        border.color: config.beautifyBackground === bgButton.index ? LingmoUI.Theme.highlightColor
                                                                                                  : LingmoUI.Theme.borderColor
                                        gradient: Gradient {
                                            orientation: Gradient.Horizontal
                                            GradientStop { position: 0; color: bgButton.modelData[0] }
                                            GradientStop { position: 1; color: bgButton.modelData[1] }
                                        }
                                    }
                                    onClicked: config.beautifyBackground = index
                                }
                            }
                        }

                        SettingRow {
                            title: qsTr("Margin")
                            Slider {
                                Layout.preferredWidth: 180
                                from: 0; to: 160; stepSize: 4
                                value: config.beautifyPadding
                                onMoved: config.beautifyPadding = value
                            }
                        }

                        SettingRow {
                            title: qsTr("Rounded corners")
                            Slider {
                                Layout.preferredWidth: 180
                                from: 0; to: 40; stepSize: 1
                                value: config.beautifyRadius
                                onMoved: config.beautifyRadius = value
                            }
                        }

                        SettingRow {
                            title: qsTr("Shadow")
                            Switch { checked: config.beautifyShadow; onToggled: config.beautifyShadow = checked }
                        }
                    }

                    // Preview
                    Rectangle {
                        id: preview
                        readonly property var colors: config.beautifyBackgrounds[config.beautifyBackground]
                        readonly property real k: 0.5
                        Layout.preferredWidth: 220
                        Layout.preferredHeight: 150
                        Layout.alignment: Qt.AlignTop
                        radius: LingmoUI.Theme.smallRadius
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0; color: preview.colors[0] }
                            GradientStop { position: 1; color: preview.colors[1] }
                        }

                        LingmoUI.Elevation {
                            visible: config.beautifyShadow && config.beautifyPadding >= 8
                            anchors.fill: shot
                            level: 2
                            radius: shot.radius
                        }

                        Rectangle {
                            id: shot
                            anchors.fill: parent
                            anchors.margins: Math.min(50, config.beautifyPadding * preview.k)
                            radius: config.beautifyRadius * preview.k
                            color: LingmoUI.Theme.secondBackgroundColor

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: LingmoUI.Units.smallSpacing
                                spacing: 4
                                Repeater {
                                    model: 4
                                    Rectangle {
                                        required property int index
                                        Layout.preferredWidth: parent.width * (index === 0 ? 0.5 : 0.9 - index * 0.12)
                                        Layout.preferredHeight: 5
                                        radius: 2
                                        color: index === 0 ? LingmoUI.Theme.highlightColor : LingmoUI.Theme.dividerColor
                                    }
                                }
                                Item { Layout.fillHeight: true }
                            }
                        }
                    }
                }
            }

            // ------------------------------------------------ history
            SectionCard {
                heading: qsTr("History")

                SettingRow {
                    title: qsTr("Keep recent captures")
                    subtitle: qsTr("Only on this computer, in a private folder.")
                    Switch { checked: config.historyEnabled; onToggled: config.historyEnabled = checked }
                }
                SettingRow {
                    title: qsTr("How many")
                    enabled: config.historyEnabled
                    SpinBox {
                        from: 1; to: 500
                        value: config.historySize
                        editable: true
                        onValueModified: config.historySize = value
                    }
                }
            }

            // ------------------------------------------------ recording
            SectionCard {
                heading: qsTr("Screen recording")

                SessionHint {
                    session: "x11"
                    visible: !app.canRecord()
                    text: qsTr("Not available in the Wayland session yet")
                    Layout.fillWidth: true
                }
                SettingRow {
                    title: qsTr("Format")
                    subtitle: qsTr("GIF has no sound.")
                    LingmoUI.SegmentedControl {
                        model: [qsTr("Video (MP4)"), qsTr("Animation (GIF)")]
                        currentIndex: config.recordFormat === "gif" ? 1 : 0
                        onActivated: (index) => config.recordFormat = index === 1 ? "gif" : "mp4"
                    }
                }
                SettingRow {
                    title: qsTr("Frames per second")
                    SpinBox {
                        from: 5; to: 60
                        value: config.recordFrameRate
                        editable: true
                        onValueModified: config.recordFrameRate = value
                    }
                }
                SettingRow {
                    title: qsTr("Microphone")
                    Switch { checked: config.recordMicrophone; onToggled: config.recordMicrophone = checked }
                }
                SettingRow {
                    title: qsTr("System audio")
                    Switch { checked: config.recordSystemAudio; onToggled: config.recordSystemAudio = checked }
                }
                SettingRow {
                    // XInput 2: X11 only
                    visible: Session.supports("x11")
                    title: qsTr("Show clicks")
                    Switch { checked: config.recordShowClicks; onToggled: config.recordShowClicks = checked }
                }
                SettingRow {
                    title: qsTr("Folder")
                    subtitle: app.recordingsFolder()
                    LingmoUI.IconButton {
                        iconName: "folder-open"
                        tipText: qsTr("Open the folder")
                        onClicked: app.openRecordingsFolder()
                    }
                }
            }

            // ------------------------------------------------ send to
            SectionCard {
                heading: qsTr("Send to…")

                LingmoUI.Banner {
                    Layout.fillWidth: true
                    closable: false
                    type: LingmoUI.Banner.Info
                    text: qsTr("For your privacy, captures are never uploaded unless you set up a service here. The command gets the image as %f and must print the link.")
                }

                SettingRow {
                    title: qsTr("Name")
                    TextField {
                        Layout.preferredWidth: 300
                        placeholderText: qsTr("My server")
                        text: config.uploadName
                        selectByMouse: true
                        onEditingFinished: config.uploadName = text
                    }
                }
                SettingRow {
                    title: qsTr("Command")
                    subtitle: config.uploadCommand.length > 0 && !config.uploadEnabled
                              ? qsTr("The command must contain %f.") : ""
                    TextField {
                        Layout.preferredWidth: 300
                        placeholderText: "curl -sF file=@%f https://…"
                        text: config.uploadCommand
                        selectByMouse: true
                        onEditingFinished: config.uploadCommand = text
                    }
                }
            }
        }
    }
}
