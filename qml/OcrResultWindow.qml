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
    property bool showTranslation: false
    // Translation over the picture needs one translation per recognized line;
    // a service that merged or split lines leaves this empty and the whole text
    // stays in the pane instead.
    readonly property bool overlayAvailable: App.ocrResult.translatedLines.length > 0

    // Preset translation targets. The setting stays free text (a language name
    // the model understands); this list exists so the window can switch target
    // without a trip to Settings, and a hand-typed value is kept as the first
    // entry instead of being replaced by a preset.
    readonly property var translationTargets: [
        "English", "Simplified Chinese", "Traditional Chinese", "Japanese",
        "Korean", "Polish", "German", "French", "Spanish", "Italian",
        "Russian", "Portuguese", "Ukrainian", "Turkish", "Vietnamese", "Thai",
        "Indonesian", "Arabic", "Hindi"
    ]
    // 100%: one capture pixel per screen pixel. A HiDPI screen reports
    // devicePixelRatio > 1, so the picture's logical size is smaller by that
    // factor - that is what "original size" means for a screenshot. The pane
    // scales below this only when the capture does not fit (a full-screen
    // shot), and then uniformly.
    readonly property real hundredScale: {
        const s = resultWindow.screen
        const dpr = s ? s.devicePixelRatio : 1
        return 1 / Math.max(1, dpr)
    }
    readonly property string targetName: App.ocrResult.translationTarget
    readonly property var targetModel: {
        const list = resultWindow.translationTargets
        return list.indexOf(resultWindow.targetName) >= 0
               ? list : [resultWindow.targetName].concat(list)
    }

    // Open wide enough that the picture is shown at 100% beside the text pane,
    // instead of shrinking a full-page capture into half the window. Clamped to
    // the screen; the picture scrolls when it still does not fit, and a window
    // the user maximized or resized by hand is left alone.
    function fitToImage() {
        const img = App.ocrResult.imageSize
        if (!img || img.width <= 0 || !resultWindow.screen)
            return
        if (resultWindow.visibility === Window.Maximized
                || resultWindow.visibility === Window.FullScreen)
            return
        const picW = img.width * resultWindow.hundredScale
        const picH = img.height * resultWindow.hundredScale
        const maxW = Math.floor(resultWindow.screen.width * 0.92)
        const maxH = Math.floor(resultWindow.screen.height * 0.92)
        resultWindow.width = Math.max(resultWindow.minimumWidth,
                                      Math.min(maxW, Math.round(picW + 440)))
        resultWindow.height = Math.max(resultWindow.minimumHeight,
                                       Math.min(maxH, Math.round(picH + 200)))
    }

    // One place for "a line on the picture selects its range in the pane", so
    // the original boxes and the translated covers behave the same.
    function selectPaneRange(start, length) {
        textEdit.select(start, start + length)
        textEdit.forceActiveFocus()
        const r = textEdit.positionToRectangle(start)
        textFlick.contentY = Math.max(0, r.y - 24)
    }

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
            resultWindow.fitToImage()
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

            // The hint keeps its own line, so the controls below keep their
            // width when the window is narrow instead of squeezing the hint
            // into a column next to them.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingM
                Text {
                    Layout.fillWidth: true
                    text: !resultWindow.showTranslation
                          ? qsTr("Click a line on the picture to select it, or select text in the pane.")
                          : resultWindow.overlayAvailable
                            ? qsTr("Click a translated line on the picture to select it, or select text in the pane.")
                            : qsTr("Translated text. Select it below or copy it all.")
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
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingM
                // The translation target, visible and switchable right here:
                // picking a new one while a translation is up re-runs it with
                // the new language (the first Translate stays an explicit
                // choice - nothing is sent just by changing the target).
                UComboBox {
                    id: targetPicker
                    objectName: "targetPicker"
                    width: 170
                    visible: App.ocrResult.canTranslate
                    enabled: !App.ocrResult.translating
                    model: resultWindow.targetModel
                    currentIndex: Math.max(0, resultWindow.targetModel.indexOf(resultWindow.targetName))
                    accessibleName: qsTr("Target language")
                    accessibleDescription: qsTr("Language name written in the prompt.")
                    onActivated: (i) => {
                        const picked = resultWindow.targetModel[i]
                        if (picked === resultWindow.targetName)
                            return
                        App.settings.translationTarget = picked
                        if (App.ocrResult.translation.length > 0)
                            App.ocrResult.translate()
                    }
                }
                // Translation controls stay together on the left, the view
                // and copy controls on the right. Everything keeps its place
                // and disables instead of disappearing: a hidden button
                // reflows the row and slides its neighbours while the pointer
                // is still down on the previous one.
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
                    enabled: App.ocrResult.translating
                    text: qsTr("Cancel")
                    onClicked: App.ocrResult.cancelTranslation()
                }
                Item { Layout.fillWidth: true }
                UButton {
                    compact: true
                    variant: "tonal"
                    enabled: textEdit.selectedText.length > 0
                    text: qsTr("Copy selection")
                    onClicked: {
                        textEdit.copy()
                        copiedLabel.opacity = 1
                        copiedTimer.restart()
                    }
                }
                Row {
                    spacing: 2
                    UButton {
                        compact: true
                        enabled: App.ocrResult.translation.length > 0
                        variant: resultWindow.showTranslation ? "tonal" : "filled"
                        text: qsTr("Original")
                        onClicked: resultWindow.showTranslation = false
                    }
                    UButton {
                        compact: true
                        enabled: App.ocrResult.translation.length > 0
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

            // A translation that did not come back one line per line is still
            // shown, in the pane; say so instead of leaving the picture bare.
            Text {
                Layout.fillWidth: true
                visible: resultWindow.showTranslation && App.ocrResult.translation !== ""
                         && !resultWindow.overlayAvailable && App.ocrResult.lines.length > 1
                text: qsTr("The service did not translate line by line, so the translation is shown in the pane instead of on the picture.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontS
                wrapMode: Text.WordWrap
            }

            // A service that answers with the recognized text itself (often a
            // language pair it handles badly) is not a translation: say so
            // instead of letting the source look like the result.
            Text {
                Layout.fillWidth: true
                visible: resultWindow.showTranslation && App.ocrResult.translationUnchanged
                text: qsTr("The service returned the text unchanged. Check the target language and the model.")
                color: Theme.textSecondary
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
                    // Side by side the picture gets its own width at 100% and
                    // the text pane takes the rest; stacked, both share the
                    // window width.
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 220
                    Layout.minimumHeight: 130
                    Layout.preferredWidth: Math.round(App.ocrResult.imageSize.width
                                                      * resultWindow.hundredScale)
                    Layout.maximumWidth: resultWindow.sideBySide
                                         ? Math.round(App.ocrResult.imageSize.width
                                                      * resultWindow.hundredScale)
                                         : 1000000
                    color: Theme.primary
                    radius: Theme.radiusM
                    clip: true

                    // The picture is drawn at its own size and only shrunk
                    // (proportionally, never past 100%) when the capture does
                    // not fit the pane - a full-screen shot, or a window the
                    // user made smaller.
                    readonly property real renderScale: {
                        const img = App.ocrResult.imageSize
                        if (img.width <= 0 || img.height <= 0)
                            return resultWindow.hundredScale
                        const fit = Math.min(width / img.width, height / img.height)
                        return Math.min(resultWindow.hundredScale, fit)
                    }

                    Item {
                        id: imageHolder
                        width: App.ocrResult.imageSize.width * imagePane.renderScale
                        height: App.ocrResult.imageSize.height * imagePane.renderScale
                        anchors.centerIn: parent

                            Image {
                                id: preview
                                anchors.fill: parent
                                source: App.ocrResult.imageSource
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                cache: false
                                smooth: true
                            }

                            // A hairline around the picture: on a light theme a
                            // white screenshot on the pane background otherwise
                            // looks like empty space instead of a capture.
                            Rectangle {
                                anchors.fill: parent
                                visible: preview.status === Image.Ready
                                color: "transparent"
                                border.width: 1
                                border.color: Theme.divider
                            }

                            Repeater {
                                model: App.ocrResult.lines
                                delegate: Rectangle {
                                    id: lineBox
                                    z: 2
                                    required property var modelData
                                    readonly property bool picked:
                                        textEdit.selectionStart < modelData.start + modelData.length
                                        && textEdit.selectionEnd > modelData.start
                                    visible: preview.status === Image.Ready
                                             && !resultWindow.showTranslation
                                    x: modelData.x * imagePane.renderScale
                                    y: modelData.y * imagePane.renderScale
                                    width: Math.max(1, modelData.width * imagePane.renderScale)
                                    height: Math.max(1, modelData.height * imagePane.renderScale)
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
                                        onClicked: resultWindow.selectPaneRange(modelData.start,
                                                                                modelData.length)
                                    }
                                }
                            }

                            // The translation layer: one opaque patch per line
                            // box, in the page colour sampled around that line,
                            // with the translation on it. Sizes, wrapping and
                            // the shrinking font keep long translations inside
                            // the original box; a click selects that line's
                            // translation in the pane.
                            Repeater {
                                model: App.ocrResult.translatedLines
                                delegate: Item {
                                    required property var modelData
                                    visible: resultWindow.showTranslation
                                             && preview.status === Image.Ready
                                    // The cover rect already carries the
                                    // overshoot pad in image pixels; half a
                                    // pixel keeps the scaled edge covered.
                                    readonly property real pad: 0.5
                                    readonly property bool picked:
                                        textEdit.selectionStart < modelData.start + modelData.length
                                        && textEdit.selectionEnd > modelData.start
                                    x: modelData.x * imagePane.renderScale - pad
                                    y: modelData.y * imagePane.renderScale - pad
                                    width: Math.max(1, modelData.width * imagePane.renderScale
                                                    + 2 * pad)
                                    height: Math.max(1, modelData.height * imagePane.renderScale
                                                     + 2 * pad)
                                    z: 3

                                    Rectangle {
                                        anchors.fill: parent
                                        color: modelData.background
                                        radius: Math.min(3, height / 4)
                                    }
                                    Text {
                                        anchors.fill: parent
                                        text: modelData.text
                                        color: modelData.foreground
                                        // Text.Fit shrinks from this size until
                                        // the translation fits the box;
                                        // minimumPixelSize keeps it from
                                        // vanishing on a small window.
                                        font.pixelSize: Math.max(8, height)
                                        fontSizeMode: Text.Fit
                                        minimumPixelSize: 5
                                        wrapMode: Text.Wrap
                                        horizontalAlignment: Text.AlignLeft
                                        verticalAlignment: Text.AlignVCenter
                                        clip: true
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: Math.min(3, height / 4)
                                        color: "transparent"
                                        border.width: 1
                                        border.color: picked ? Theme.accent
                                                             : Theme.alpha(Theme.accent, 0.85)
                                        visible: picked || coverHover.containsMouse
                                    }
                                    MouseArea {
                                        id: coverHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.IBeamCursor
                                        onClicked: resultWindow.selectPaneRange(modelData.start,
                                                                                modelData.length)
                                    }
                                }
                            }
                        }
                    }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 300
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
