import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls
import Unisic
import Unisic.Kit
import "components"

// The OCR result, with the screenshot kept in view. The picture is the
// untouched capture; every recognized line is clickable and selects its own
// range in the text pane, where normal mouse selection and Ctrl+C work. The
// pane is the same text the Copy all button puts on the clipboard, so the two
// can never disagree.
Window {
    id: resultWindow
    width: Math.min(1120, Math.floor(Screen.width * 0.86))
    height: Math.min(800, Math.floor(Screen.height * 0.86))
    minimumWidth: 640
    minimumHeight: 460
    visible: false
    title: qsTr("Recognized text")
    color: Theme.background
    flags: App.settings.useSystemDecoration
           ? Qt.Window
           : (Qt.Window | Qt.FramelessWindowHint)
    readonly property int chromeTop: App.settings.useSystemDecoration ? 0 : 38
    // Side by side on a normal desktop window; stacked once it gets narrow,
    // so neither pane squeezes the picture into a strip.
    readonly property bool sideBySide: width >= 820
    // The translated pane has no per-line geometry, so the clickable line
    // boxes belong to the original view only.
    property bool showTranslation: false

    onVisibleChanged: {
        if (visible)
            textEdit.forceActiveFocus()
    }
    onClosing: App.ocrResult.cancelTranslation()

    Shortcut {
        sequence: "Esc"
        onActivated: resultWindow.close()
    }

    Connections {
        target: App.ocrResult
        function onResultChanged() {
            resultWindow.showTranslation = false
        }
        function onTranslationReady() {
            resultWindow.showTranslation = true
        }
        function onTranslationChanged() {
            if (App.ocrResult.translation.length === 0 && !App.ocrResult.translating)
                resultWindow.showTranslation = false
        }
    }

    // ---------- custom title bar (frameless decoration) ----------
    Rectangle {
        id: titleBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: resultWindow.chromeTop
        visible: !App.settings.useSystemDecoration
        z: 20
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.lighter(Theme.primary, 1.12) }
            GradientStop { position: 1.0; color: Theme.primary }
        }
        MouseArea {
            anchors.fill: parent
            property real pressX: 0
            property real pressY: 0
            property bool moving: false
            onPressed: (m) => { pressX = m.x; pressY = m.y; moving = false }
            onPositionChanged: (m) => {
                if (!moving && (Math.abs(m.x - pressX) > 6 || Math.abs(m.y - pressY) > 6)) {
                    moving = true
                    resultWindow.startSystemMove()
                }
            }
            onDoubleClicked: resultWindow.visibility === Window.Maximized
                             ? resultWindow.showNormal() : resultWindow.showMaximized()
        }
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacingL
            anchors.verticalCenter: parent.verticalCenter
            text: resultWindow.title
            color: Theme.textPrimary
            font.pixelSize: Theme.fontM
            font.weight: Font.DemiBold
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            UIconButton {
                iconName: "minus"; iconSize: 14; width: 30; height: 30
                tooltip: qsTr("Minimize")
                onClicked: resultWindow.showMinimized()
            }
            UIconButton {
                iconName: "window"; iconSize: 13; width: 30; height: 30
                tooltip: qsTr("Maximize")
                onClicked: resultWindow.visibility === Window.Maximized
                           ? resultWindow.showNormal() : resultWindow.showMaximized()
            }
            UIconButton {
                iconName: "close"; iconSize: 14; width: 30; height: 30
                tooltip: qsTr("Close")
                onClicked: resultWindow.close()
            }
        }
    }

    UCard {
        anchors.fill: parent
        anchors.margins: Theme.spacingL
        anchors.topMargin: Theme.spacingL + resultWindow.chromeTop

        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.spacingM

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingM
                Text {
                    Layout.fillWidth: true
                    text: resultWindow.showTranslation
                          ? qsTr("Translated text. Select it below or copy it all.")
                          : qsTr("Click a line on the picture to select it, or select text in the pane.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontS
                    wrapMode: Text.WordWrap
                }
                Text {
                    id: copiedLabel
                    text: qsTr("Copied")
                    color: Theme.accent
                    font.pixelSize: Theme.fontS
                    font.weight: Font.DemiBold
                    opacity: 0
                    Behavior on opacity { NumberAnimation { duration: 120 } }
                }
                UButton {
                    compact: true
                    variant: "tonal"
                    visible: textEdit.selectedText.length > 0
                    text: qsTr("Copy selection")
                    onClicked: {
                        textEdit.copy()
                        copiedLabel.opacity = 1
                        copiedTimer.restart()
                    }
                }
                UButton {
                    compact: true
                    variant: "tonal"
                    text: App.ocrResult.translating ? qsTr("Translating…") : qsTr("Translate")
                    accessibleDescription: App.ocrResult.canTranslate
                                           ? qsTr("Translate the recognized text")
                                           : qsTr("Configure translation in Settings > General")
                    enabled: !App.ocrResult.translating
                    onClicked: App.ocrResult.translate()
                }
                UButton {
                    compact: true
                    variant: "tonal"
                    visible: App.ocrResult.translating
                    text: qsTr("Cancel")
                    onClicked: App.ocrResult.cancelTranslation()
                }
                Row {
                    visible: App.ocrResult.translation.length > 0
                    spacing: 2
                    UButton {
                        compact: true
                        variant: resultWindow.showTranslation ? "tonal" : "filled"
                        text: qsTr("Original")
                        onClicked: resultWindow.showTranslation = false
                    }
                    UButton {
                        compact: true
                        variant: resultWindow.showTranslation ? "filled" : "tonal"
                        text: qsTr("Translation")
                        onClicked: resultWindow.showTranslation = true
                    }
                }
                UButton {
                    compact: true
                    variant: "filled"
                    iconName: "edit-copy"
                    text: qsTr("Copy all")
                    onClicked: {
                        App.copyText(resultWindow.showTranslation
                                     ? App.ocrResult.translation : App.ocrResult.text)
                        copiedLabel.opacity = 1
                        copiedTimer.restart()
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: App.ocrResult.translationError !== ""
                text: App.ocrResult.translationError
                color: Theme.danger
                font.pixelSize: Theme.fontS
                wrapMode: Text.WordWrap
            }

            GridLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                columns: resultWindow.sideBySide ? 2 : 1
                rows: resultWindow.sideBySide ? 1 : 2
                columnSpacing: Theme.spacingM
                rowSpacing: Theme.spacingM

                Rectangle {
                    id: imagePane
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 220
                    Layout.minimumHeight: 130
                    color: Theme.primary
                    radius: Theme.radiusM
                    clip: true

                    Image {
                        id: preview
                        anchors.fill: parent
                        source: App.ocrResult.imageSource
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                        smooth: true
                    }

                    readonly property real picScale: preview.paintedWidth
                                                     / Math.max(1, App.ocrResult.imageSize.width)

                    Repeater {
                        model: App.ocrResult.lines
                        delegate: Rectangle {
                            id: lineBox
                            z: 2
                            required property var modelData
                            readonly property bool picked:
                                textEdit.selectionStart < modelData.start + modelData.length
                                && textEdit.selectionEnd > modelData.start
                            visible: preview.status === Image.Ready && imagePane.picScale > 0
                                     && !resultWindow.showTranslation
                            x: (imagePane.width - preview.paintedWidth) / 2
                               + modelData.x * imagePane.picScale
                            y: (imagePane.height - preview.paintedHeight) / 2
                               + modelData.y * imagePane.picScale
                            width: Math.max(1, modelData.width * imagePane.picScale)
                            height: Math.max(1, modelData.height * imagePane.picScale)
                            radius: 2
                            color: picked ? Theme.alpha(Theme.accent, 0.32)
                                 : hoverArea.containsMouse
                                   ? Theme.alpha(Theme.accent, 0.18)
                                   : Theme.alpha(Theme.accent, 0.08)
                            border.width: 1
                            border.color: picked ? Theme.accent
                                                 : Theme.alpha(Theme.accent, 0.8)
                            MouseArea {
                                id: hoverArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.IBeamCursor
                                onClicked: {
                                    textEdit.select(modelData.start,
                                                    modelData.start + modelData.length)
                                    textEdit.forceActiveFocus()
                                    const r = textEdit.positionToRectangle(modelData.start)
                                    textFlick.contentY = Math.max(0, r.y - 24)
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 220
                    Layout.minimumHeight: 130
                    color: Theme.secondary
                    radius: Theme.radiusM

                    Flickable {
                        id: textFlick
                        anchors.fill: parent
                        anchors.margins: Theme.spacingM
                        contentWidth: width
                        contentHeight: textEdit.contentHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        TextEdit {
                            id: textEdit
                            width: textFlick.width
                            rightPadding: 10
                            text: resultWindow.showTranslation
                                  ? App.ocrResult.translation : App.ocrResult.text
                            readOnly: true
                            selectByMouse: true
                            persistentSelection: true
                            wrapMode: TextEdit.Wrap
                            color: Theme.textPrimary
                            selectionColor: Theme.accent
                            selectedTextColor: Theme.primary
                            font.pixelSize: Theme.fontM
                        }
                    }
                }
            }
        }
    }

    Timer {
        id: copiedTimer
        interval: 1500
        onTriggered: copiedLabel.opacity = 0
    }

    UWindowResizer { active: !App.settings.useSystemDecoration }
}
