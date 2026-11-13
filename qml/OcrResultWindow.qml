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
    // The text-controls row sets the floor: it cannot shrink below its buttons
    // (the target picker adds ~180 more once translation is configured, which
    // is why a configured window wants ~980), and a narrower window used to
    // push the picture row and the zoom row past the card's edge with it.
    minimumWidth: 720
    // The chrome above the panes (hint, text controls, picture actions, zoom)
    // plus two stacked panes at their own minimum: any shorter and the stacked
    // split cannot give either pane a usable strip.
    minimumHeight: 560
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
    // Set while the picture drives the pane's native selection, so a
    // cursorPositionChanged from our own select() call does not clear the
    // line-box multi-selection (a real drag in the pane does clear it).
    property bool syncingPaneSelection: false
    // Translation over the picture needs one translation per recognized line;
    // a service that merged or split lines leaves this empty and the whole text
    // stays in the pane instead.
    readonly property bool overlayAvailable: App.ocrResult.translatedLines.length > 0
    // Covers whose translation could not fit even at the 5 px floor. They are
    // drawn shortened (and marked on the picture); the window says so once
    // instead of letting the reader wonder what was cut.
    readonly property int overflowLines: {
        let count = 0
        const list = App.ocrResult.translatedLines
        for (let i = 0; i < list.length; ++i) {
            if (list[i].overflow)
                ++count
        }
        return count
    }

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
        // The chrome above the panes: the hint, the text controls, the picture
        // actions and the zoom row, plus the card's own margins. It grew with
        // M2, and a window sized for 100% that then shows 80% makes the zoom
        // controls look like they did something on open.
        resultWindow.height = Math.max(resultWindow.minimumHeight,
                                       Math.min(maxH, Math.round(picH + 300)))
    }

    // One place for "a line on the picture selects its range in the pane", so
    // the original boxes and the translated covers behave the same.
    function selectPaneRange(start, length) {
        resultWindow.syncingPaneSelection = true
        textEdit.select(start, start + length)
        resultWindow.syncingPaneSelection = false
        textEdit.forceActiveFocus()
        const r = textEdit.positionToRectangle(start)
        textFlick.contentY = Math.max(0, r.y - 24)
    }

    // The source line behind a translated cover. The cover map can skip a line
    // whose chunk failed, so a cover's index is not a source line index; the
    // source range it carries is the link.
    function sourceLineOfCover(entry) {
        if (!entry || entry.sourceStart === undefined || entry.sourceStart < 0)
            return -1
        const lines = App.ocrResult.lines
        for (let i = 0; i < lines.length; ++i) {
            if (entry.sourceStart >= lines[i].start
                    && entry.sourceStart < lines[i].start + lines[i].length)
                return i
        }
        return -1
    }

    // A click on the picture, resolved through the pane's own hit test (image
    // coordinates), so Original and Translation select the same way at any zoom.
    // Plain: one line. Ctrl: add or remove it from the set. Shift: a range from
    // the anchor. The pane keeps its native selection for the partial-text
    // translation path (T4.5.2).
    function clickPicture(px, py, modifiers) {
        const index = imagePane.lineIndexAt(px, py)
        if (index < 0) {
            if (!(modifiers & Qt.ControlModifier))
                App.ocrResult.clearLineSelection()
            return
        }
        const entry = resultWindow.showTranslation
                      ? App.ocrResult.translatedLines[index]
                      : App.ocrResult.lines[index]
        if (!entry)
            return
        const sourceIndex = resultWindow.showTranslation
                            ? resultWindow.sourceLineOfCover(entry) : index
        if (modifiers & Qt.ControlModifier) {
            if (sourceIndex >= 0)
                App.ocrResult.toggleLine(sourceIndex)
            return
        }
        if (modifiers & Qt.ShiftModifier) {
            if (sourceIndex < 0)
                return
            App.ocrResult.extendLineSelection(sourceIndex)
            const lines = App.ocrResult.selectedLines
            if (lines.length > 0) {
                const first = App.ocrResult.lines[lines[0]]
                const last = App.ocrResult.lines[lines[lines.length - 1]]
                resultWindow.selectPaneRange(first.start,
                                             last.start + last.length - first.start)
            }
            return
        }
        if (sourceIndex >= 0)
            App.ocrResult.selectLine(sourceIndex)
        resultWindow.selectPaneRange(entry.start, entry.length)
    }

    // The source-text range the current pane selection stands for. On the
    // Original layer the selection IS source text; on the Translation layer
    // every translated cover carries the source range it came from, so a
    // selection made there can still re-translate exactly those source lines.
    function selectedSourceRange() {
        // The binding can be evaluated before the pane exists (the Translate
        // button's label); no pane means no selection.
        if (!textEdit || textEdit.selectedText.length === 0)
            return { start: -1, length: 0 }
        if (!resultWindow.showTranslation)
            return { start: textEdit.selectionStart, length: textEdit.selectedText.length }
        let start = -1
        let end = -1
        const list = App.ocrResult.translatedLines
        for (let i = 0; i < list.length; ++i) {
            const entry = list[i]
            if (textEdit.selectionStart >= entry.start + entry.length
                    || textEdit.selectionEnd <= entry.start)
                continue
            if (entry.sourceStart === undefined || entry.sourceStart < 0)
                continue
            start = start < 0 ? entry.sourceStart : Math.min(start, entry.sourceStart)
            end = Math.max(end, entry.sourceStart + entry.sourceLength)
        }
        return start >= 0 ? { start: start, length: end - start }
                          : { start: -1, length: 0 }
    }

    // The chosen lines as a lookup object for the delegates. Rebuilt whenever
    // the controller's list changes, so a box and a cover share one state.
    readonly property var selectedLineSet: {
        const set = {}
        const list = App.ocrResult.selectedLines
        for (let i = 0; i < list.length; ++i)
            set[list[i]] = true
        return set
    }

    // True when the pane selection is not exactly whole lines: it translates
    // that exact text into the pane and must not cover the line it came from.
    readonly property bool partialSelection: {
        const range = resultWindow.selectedSourceRange()
        return range.length > 0
               && !App.ocrResult.rangeCoversWholeLines(range.start, range.length)
    }

    // How many recognized lines the Translate button would send. A line-box set
    // wins over the native selection (they are separate models); otherwise the
    // selection is widened to the boxes it touches, exactly like M3.
    readonly property int selectedLineCount: {
        if (App.ocrResult.selectedLines.length > 0)
            return App.ocrResult.selectedLines.length
        const range = resultWindow.selectedSourceRange()
        if (range.length <= 0)
            return 0
        let count = 0
        const lines = App.ocrResult.lines
        for (let i = 0; i < lines.length; ++i) {
            const line = lines[i]
            if (range.start < line.start + line.length
                    && range.start + range.length > line.start)
                ++count
        }
        return count
    }

    function translateFromPane() {
        // Resolve the selection against the layer the user is looking at,
        // before switching to Translation: the offsets belong to that text.
        const range = resultWindow.selectedSourceRange()
        // Switch to the Translation layer before the first row arrives, or a
        // streaming answer would keep showing the source text until the end.
        resultWindow.showTranslation = true
        if (App.ocrResult.selectedLines.length > 0)
            App.ocrResult.translateSelectedLines()
        else if (range.length > 0)
            App.ocrResult.translateRange(range.start, range.length)
        else
            App.ocrResult.translate()
    }

    onVisibleChanged: {
        if (visible)
            textEdit.forceActiveFocus()
    }
    onClosing: App.ocrResult.cancelTranslation()

    // Esc clears the line-box set first and closes only when there is none, so
    // picking several lines never traps the user in the window.
    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (privacyNotice.visible) {
                privacyNotice.close()
                return
            }
            if (App.ocrResult.selectedLines.length > 0)
                App.ocrResult.clearLineSelection()
            else
                resultWindow.close()
        }
    }
    // Ctrl+A means "all lines" here: the same set the picture boxes use, and the
    // native pane selection follows so Copy/Ctrl+C agree.
    Shortcut {
        sequence: "Ctrl+A"
        onActivated: {
            resultWindow.syncingPaneSelection = true
            textEdit.selectAll()
            resultWindow.syncingPaneSelection = false
            App.ocrResult.selectAllLines()
        }
    }

    Connections {
        target: App.ocrResult
        function onResultChanged() {
            resultWindow.showTranslation = false
            // A new capture starts fitted and centred: carrying the previous
            // page's 200% over to it would open on a corner of the new one.
            imagePane.showFit()
            imagePane.hoverIndex = -1
            resultWindow.fitToImage()
        }
        function onTranslationReady() {
            resultWindow.showTranslation = true
            imagePane.hoverIndex = -1
        }
        function onTranslationChanged() {
            imagePane.hoverIndex = -1
            if (App.ocrResult.translation.length === 0 && !App.ocrResult.translating)
                resultWindow.showTranslation = false
        }
        // T4.3: the first request to a non-local host waits for this answer.
        // Nothing has been sent yet; local endpoints never get here.
        function onPrivacyNoticeRequested() {
            privacyNotice.open()
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
                          ? qsTr("Click a line on the picture to select it, Ctrl+click to add lines, Shift+click for a range, or select text in the pane to translate that text.")
                          : resultWindow.overlayAvailable
                            ? qsTr("Click a translated line on the picture to select it, Ctrl+click to add lines, or select text in the pane.")
                            : qsTr("Translated text. Select it below or copy it all.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontS
                    wrapMode: Text.WordWrap
                }
                // Confirmation for Copy all, Copy image, Save image and Edit in
                // editor. It belongs to this window and not to a toast: toasts
                // live in the main window, which the result window is usually
                // covering while the user works here. The controller clears the
                // text after a couple of seconds.
                Text {
                    id: statusLabel
                    text: App.ocrResult.statusText
                    color: App.ocrResult.statusIsError ? Theme.danger : Theme.accent
                    font.pixelSize: Theme.fontS
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    Layout.preferredWidth: Math.min(implicitWidth, 340)
                    opacity: text !== "" ? 1 : 0
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
                            resultWindow.translateFromPane()
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
                    text: App.ocrResult.translating
                          ? (App.ocrResult.translationChunkCount > 1
                             ? qsTr("Translating… %1/%2")
                                   .arg(Math.max(1, App.ocrResult.translationChunkIndex))
                                   .arg(App.ocrResult.translationChunkCount)
                             : qsTr("Translating…"))
                          : resultWindow.selectedLineCount > 0
                            ? qsTr("Translate selection")
                            : qsTr("Translate all")
                    accessibleDescription: App.ocrResult.canTranslate
                                           ? (resultWindow.partialSelection
                                              ? qsTr("Translate only the selected text")
                                              : resultWindow.selectedLineCount > 0
                                                ? qsTr("Translate only the selected lines")
                                                : qsTr("Translate the recognized text"))
                                           : qsTr("Configure translation in Settings > General")
                    enabled: !App.ocrResult.translating
                    onClicked: resultWindow.translateFromPane()
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
                    // The line-box set wins: Copy selection takes those lines in
                    // document order. With no set it copies the native pane
                    // selection, partial text included.
                    enabled: App.ocrResult.selectedLines.length > 0
                             || textEdit.selectedText.length > 0
                    text: qsTr("Copy selection")
                    onClicked: {
                        if (App.ocrResult.selectedLines.length > 0)
                            App.copyText(App.ocrResult.selectionText())
                        else
                            textEdit.copy()
                        App.ocrResult.setStatusText(qsTr("Copied"), false)
                    }
                }
                Row {
                    spacing: 2
                    UButton {
                        compact: true
                        enabled: App.ocrResult.translation.length > 0
                        variant: resultWindow.showTranslation ? "tonal" : "filled"
                        text: qsTr("Original")
                        onClicked: {
                        resultWindow.showTranslation = false
                        imagePane.hoverIndex = -1
                    }
                    }
                    UButton {
                        compact: true
                        enabled: App.ocrResult.translation.length > 0
                        variant: resultWindow.showTranslation ? "filled" : "tonal"
                        text: qsTr("Translation")
                        onClicked: {
                        resultWindow.showTranslation = true
                        imagePane.hoverIndex = -1
                    }
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
                        App.ocrResult.setStatusText(qsTr("Copied"), false)
                    }
                }
            }

            // Picture actions. They act on the layer on show (Original or
            // Translation) and always on the capture's own resolution, never on
            // the scaled-down view the window may be drawing.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingM
                UButton {
                    compact: true
                    variant: "tonal"
                    iconName: "document-save"
                    text: qsTr("Save image…")
                    enabled: App.ocrResult.imageSize.width > 0
                    accessibleDescription: qsTr("Saves the picture on show, at the capture's own size")
                    onClicked: App.saveOcrResultImage(resultWindow.showTranslation)
                }
                UButton {
                    compact: true
                    variant: "tonal"
                    iconName: "edit-copy"
                    text: qsTr("Copy image")
                    enabled: App.ocrResult.imageSize.width > 0
                    accessibleDescription: qsTr("Copies the picture on show to the clipboard")
                    onClicked: App.copyOcrResultImage(resultWindow.showTranslation)
                }
                UButton {
                    compact: true
                    variant: "tonal"
                    iconName: "edit"
                    text: qsTr("Edit in editor")
                    enabled: App.ocrResult.imageSize.width > 0
                    accessibleDescription: qsTr("Opens the picture on show in the editor")
                    onClicked: App.editOcrResultImage(resultWindow.showTranslation)
                }
                Item { Layout.fillWidth: true }
            }

            // Picture view: the row above acts on the FILE, this one on what the
            // pane shows. Its own row, right-aligned, so a long translated label
            // can never squeeze the zoom controls out of the window.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingS
                Item { Layout.fillWidth: true }
                UIconButton {
                    iconName: "minus"; iconSize: 14
                    width: 30; height: 30
                    tooltip: qsTr("Zoom out")
                    enabled: imagePane.effectiveZoom > 0.11
                    onClicked: imagePane.zoomBy(0.8)
                }
                UButton {
                    compact: true
                    // The label IS the current level, and pressing it goes back
                    // to 100%: one control for "where am I" and "back to normal".
                    // A fixed width keeps the buttons beside it still while the
                    // number changes under the pointer.
                    Layout.preferredWidth: 64
                    variant: Math.abs(imagePane.effectiveZoom - 1) < 0.001 ? "filled" : "tonal"
                    text: Math.round(imagePane.effectiveZoom * 100) + "%"
                    enabled: App.ocrResult.imageSize.width > 0
                    accessibleName: qsTr("Zoom level %1 percent")
                                        .arg(Math.round(imagePane.effectiveZoom * 100))
                    accessibleDescription: qsTr("Press to show the picture at 100 percent")
                    onClicked: imagePane.showHundred()
                }
                UIconButton {
                    iconName: "magnify"; iconSize: 14
                    width: 30; height: 30
                    tooltip: qsTr("Zoom in")
                    enabled: imagePane.effectiveZoom < 7.9
                    onClicked: imagePane.zoomBy(1.25)
                }
                UIconButton {
                    iconName: "transform-scale"; iconSize: 14
                    width: 30; height: 30
                    tooltip: qsTr("Fit the picture")
                    active: imagePane.autoFit
                    onClicked: imagePane.showFit()
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

            // A selection-only translation is not the whole page: say so, or
            // the reader may think the rest was translated and came back empty.
            Text {
                Layout.fillWidth: true
                visible: resultWindow.showTranslation && App.ocrResult.translationSelectionOnly
                text: qsTr("Only the selected lines are translated. Press Translate all to send the rest.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontS
                wrapMode: Text.WordWrap
            }

            // A word or part of a line is translated into the pane only, on
            // purpose: a cover over the whole line would misrepresent the page.
            Text {
                Layout.fillWidth: true
                visible: resultWindow.showTranslation && App.ocrResult.translationPartialText
                text: qsTr("Translated the selected text. A word or part of a line stays in the pane and is not laid over the picture.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontS
                wrapMode: Text.WordWrap
            }

            // Long lines are split before they are sent; say so, so a line
            // that breaks somewhere new does not look truncated.
            Text {
                Layout.fillWidth: true
                visible: resultWindow.showTranslation && App.ocrResult.translationSplitLines > 0
                text: qsTr("Some lines were too long for one request, so they were split at sentence boundaries before translating.")
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

            // A translation too long for a small line box is shortened on the
            // picture (the corner dot marks it) and complete in the pane; say
            // which, so nothing looks silently cut.
            Text {
                Layout.fillWidth: true
                visible: resultWindow.showTranslation && resultWindow.overflowLines > 0
                text: qsTr("Some lines are too long for their box at the smallest size, so the picture shows them shortened. The full translation is in the text pane.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontS
                wrapMode: Text.WordWrap
            }

            // The two panes share one strip: side by side the picture takes
            // `splitRatio` of the width, stacked the same share of the height,
            // with a draggable divider between them. The ratio is remembered
            // (Settings), and 0 means "auto" - the share that shows the capture
            // at 100%, which is how the window opened before there was a handle.
            Item {
                id: panes
                Layout.fillWidth: true
                Layout.fillHeight: true

                readonly property int handleSize: 10
                readonly property real availableW: Math.max(1, width - handleSize)
                readonly property real availableH: Math.max(1, height - handleSize)
                // The picture may not squeeze the text pane, or the other way
                // round: the clamps are in pixels so 200% zoom cannot eat the
                // text and a narrow window cannot strand a strip of picture.
                readonly property real minPicture: resultWindow.sideBySide ? 220 : 130
                readonly property real minText: resultWindow.sideBySide ? 300 : 130
                readonly property real loRatio: {
                    const avail = resultWindow.sideBySide ? availableW : availableH
                    return Math.max(0.1, Math.min(0.9, minPicture / avail))
                }
                readonly property real hiRatio: {
                    const avail = resultWindow.sideBySide ? availableW : availableH
                    return Math.max(0.1, Math.min(0.9, 1 - minText / avail))
                }
                // A window too small for both minimums has no legal ratio; it
                // gets an even split rather than a divider outside its bounds.
                readonly property bool splitFits: hiRatio > loRatio
                readonly property real splitRatio: {
                    const lo = splitFits ? loRatio : 0.5
                    const hi = splitFits ? hiRatio : 0.5
                    if (App.settings.ocrResultSplit > 0)
                        return Math.max(lo, Math.min(hi, App.settings.ocrResultSplit))
                    const img = App.ocrResult.imageSize
                    if (img.width <= 0 || img.height <= 0)
                        return (lo + hi) / 2
                    // Auto: exactly the share that shows the capture at 100%,
                    // which is how the window opened before the divider existed.
                    const scale = resultWindow.hundredScale
                    const want = resultWindow.sideBySide
                                 ? (img.width * scale) / availableW
                                 : (img.height * scale) / availableH
                    return Math.max(lo, Math.min(hi, want))
                }
                readonly property real pictureW: resultWindow.sideBySide
                                                 ? Math.round(availableW * splitRatio)
                                                 : width
                readonly property real pictureH: resultWindow.sideBySide
                                                 ? height
                                                 : Math.round(availableH * splitRatio)
                readonly property real textX: resultWindow.sideBySide
                                              ? pictureW + handleSize : 0
                readonly property real textY: resultWindow.sideBySide
                                              ? 0 : pictureH + handleSize
                readonly property real textW: resultWindow.sideBySide
                                              ? Math.max(1, width - textX) : width
                readonly property real textH: resultWindow.sideBySide
                                              ? height : Math.max(1, height - textY)
                function setSplit(ratio) {
                    const lo = splitFits ? loRatio : 0.5
                    const hi = splitFits ? hiRatio : 0.5
                    App.settings.ocrResultSplit = Math.max(lo, Math.min(hi, ratio))
                }

                Rectangle {
                    id: imagePane
                    x: 0
                    y: 0
                    width: panes.pictureW
                    height: panes.pictureH
                    color: Theme.primary
                    radius: Theme.radiusM
                    clip: true

                    // ---- view state ----
                    // 100% is one capture pixel per screen pixel (divided by the
                    // screen's DPR). `autoFit` shows the whole capture, at 100%
                    // when it fits and scaled down when it does not, and is what
                    // the window opens with; the buttons and Ctrl+wheel switch to
                    // an explicit factor. Line boxes, covers and their text are
                    // laid out in DEVICE pixels from `renderScale` - the text is
                    // re-rendered rather than raster-scaled, and a click lands on
                    // the right line at every level.
                    property bool autoFit: true
                    property real zoomFactor: 1
                    readonly property real fitFactor: {
                        const img = App.ocrResult.imageSize
                        if (img.width <= 0 || img.height <= 0 || width <= 0 || height <= 0)
                            return 1
                        return Math.min(1, width / (img.width * resultWindow.hundredScale),
                                           height / (img.height * resultWindow.hundredScale))
                    }
                    readonly property real effectiveZoom: autoFit ? fitFactor : zoomFactor
                    readonly property real renderScale: resultWindow.hundredScale * effectiveZoom

                    // Pan is an offset from the centred position, clamped to the
                    // overflow, so the picture can never be dragged off the pane
                    // and sits centred whenever it all fits.
                    property real panX: 0
                    property real panY: 0
                    readonly property real maxPanX: Math.max(0, (imageHolder.width - width) / 2)
                    readonly property real maxPanY: Math.max(0, (imageHolder.height - height) / 2)
                    readonly property real holderX: (width - imageHolder.width) / 2
                                                    + Math.max(-maxPanX, Math.min(maxPanX, panX))
                    readonly property real holderY: (height - imageHolder.height) / 2
                                                    + Math.max(-maxPanY, Math.min(maxPanY, panY))
                    // The line under the pointer, from the same hit test a click
                    // uses; -1 over the page between the lines.
                    property int hoverIndex: -1

                    function imagePointAt(px, py) {
                        return Qt.point((px - holderX) / renderScale,
                                        (py - holderY) / renderScale)
                    }
                    function lineIndexAt(px, py) {
                        if (App.ocrResult.imageSize.width <= 0 || renderScale <= 0)
                            return -1
                        return App.ocrResult.lineAt(imagePointAt(px, py),
                                                    resultWindow.showTranslation)
                    }
                    // Zoom around a pane point, keeping the image pixel under it
                    // still: that is what makes Ctrl+wheel feel like a map
                    // rather than a slideshow.
                    function zoomTo(factor, anchorX, anchorY) {
                        const img = App.ocrResult.imageSize
                        if (img.width <= 0 || img.height <= 0)
                            return
                        const oldScale = renderScale
                        const imgX = (anchorX - holderX) / oldScale
                        const imgY = (anchorY - holderY) / oldScale
                        const next = Math.max(0.1, Math.min(8, factor))
                        autoFit = false
                        zoomFactor = next
                        const scale = resultWindow.hundredScale * next
                        const w = img.width * scale
                        const h = img.height * scale
                        const maxX = Math.max(0, (w - width) / 2)
                        const maxY = Math.max(0, (h - height) / 2)
                        panX = Math.max(-maxX, Math.min(maxX,
                                                        anchorX - imgX * scale - (width - w) / 2))
                        panY = Math.max(-maxY, Math.min(maxY,
                                                        anchorY - imgY * scale - (height - h) / 2))
                    }
                    function zoomBy(step) {
                        zoomTo(effectiveZoom * step, width / 2, height / 2)
                    }
                    function showHundred() {
                        zoomTo(1, width / 2, height / 2)
                    }
                    function showFit() {
                        autoFit = true
                        panX = 0
                        panY = 0
                    }

                    Item {
                        id: imageHolder
                        width: App.ocrResult.imageSize.width * imagePane.renderScale
                        height: App.ocrResult.imageSize.height * imagePane.renderScale
                        x: imagePane.holderX
                        y: imagePane.holderY

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
                                    required property int index
                                    // The line-box set (Ctrl/Shift clicks) wins
                                    // over the native selection when it is not
                                    // empty; otherwise a pane selection still
                                    // tints the boxes it touches.
                                    readonly property bool picked:
                                        App.ocrResult.selectedLines.length > 0
                                        ? resultWindow.selectedLineSet[index] === true
                                        : (textEdit.selectionStart < modelData.start + modelData.length
                                           && textEdit.selectionEnd > modelData.start)
                                    readonly property bool hovered:
                                        imagePane.hoverIndex === index
                                        && !resultWindow.showTranslation
                                    visible: preview.status === Image.Ready
                                             && !resultWindow.showTranslation
                                    Accessible.role: Accessible.ListItem
                                    Accessible.name: modelData.text
                                    Accessible.description: qsTr("Recognized line on the picture")
                                    Accessible.selected: picked
                                    x: modelData.x * imagePane.renderScale
                                    y: modelData.y * imagePane.renderScale
                                    width: Math.max(1, modelData.width * imagePane.renderScale)
                                    height: Math.max(1, modelData.height * imagePane.renderScale)
                                    radius: 2
                                    color: picked ? Theme.alpha(Theme.accent, 0.32)
                                         : hovered ? Theme.alpha(Theme.accent, 0.18)
                                         : Theme.alpha(Theme.accent, 0.08)
                                    border.width: 1
                                    border.color: picked ? Theme.accent
                                                         : Theme.alpha(Theme.accent, 0.8)
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
                                    required property int index
                                    visible: resultWindow.showTranslation
                                             && preview.status === Image.Ready
                                    // The cover rect already carries the
                                    // overshoot pad in image pixels; half a
                                    // pixel keeps the scaled edge covered.
                                    readonly property real pad: 0.5
                                    // A cover stands for a source line; the set
                                    // stores source indices, so map back before
                                    // asking whether it is picked.
                                    readonly property int sourceIndex:
                                        resultWindow.sourceLineOfCover(modelData)
                                    readonly property bool picked:
                                        App.ocrResult.selectedLines.length > 0
                                        ? (sourceIndex >= 0
                                           && resultWindow.selectedLineSet[sourceIndex] === true)
                                        : (textEdit.selectionStart < modelData.start + modelData.length
                                           && textEdit.selectionEnd > modelData.start)
                                    readonly property bool hovered: imagePane.hoverIndex === index
                                    Accessible.role: Accessible.ListItem
                                    Accessible.name: modelData.text
                                    Accessible.description: qsTr("Translated line on the picture")
                                    Accessible.selected: picked
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
                                        // The fitted size comes from C++ in
                                        // IMAGE pixels, and it is the number the
                                        // saved picture draws at; the window only
                                        // scales it. Text.Fit here would be a
                                        // second, divergent fit - which is what
                                        // made a saved PNG differ from the screen.
                                        font.family: Qt.application.font.family
                                        font.pixelSize: Math.max(1, modelData.fontSize
                                                                    * imagePane.renderScale)
                                        wrapMode: Text.Wrap
                                        // An RTL translation sits against the
                                        // right edge of its cover, like an RTL
                                        // line in the pane. The glyph order is
                                        // the bidi algorithm's (Text has no
                                        // layoutDirection property), so the
                                        // alignment is what an RTL line needs
                                        // from here.
                                        horizontalAlignment: modelData.rtl ? Text.AlignRight
                                                                           : Text.AlignLeft
                                        verticalAlignment: Text.AlignVCenter
                                        clip: true
                                    }
                                    // A translation that could not fit even at
                                    // the 5 px floor is clipped by the cover;
                                    // the same corner dot the saved picture gets
                                    // says so instead of hiding it.
                                    Rectangle {
                                        visible: modelData.overflow
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        // Off the cover's height, never its own:
                                        // width from height and height from width
                                        // is a binding loop.
                                        anchors.margins: Math.max(1, Math.round(parent.height / 12))
                                        width: Math.max(3, Math.round(parent.height / 5))
                                        height: width
                                        radius: 1
                                        color: modelData.foreground
                                        opacity: 0.8
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: Math.min(3, height / 4)
                                        color: "transparent"
                                        border.width: 1
                                        border.color: picked ? Theme.accent
                                                             : Theme.alpha(Theme.accent, 0.85)
                                        visible: picked || hovered
                                    }
                                }
                            }
                        }

                    // One input surface for the picture: hover, click, drag to
                    // pan and Ctrl+wheel to zoom. It replaces per-line mouse
                    // areas on purpose - a click is resolved against the image
                    // coordinates, so it stays exact when the picture is panned
                    // and zoomed instead of drifting with each delegate's
                    // transformed hit area.
                    MouseArea {
                        id: imageMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        cursorShape: imageMouse.panning ? Qt.ClosedHandCursor
                                     : imagePane.hoverIndex >= 0 ? Qt.IBeamCursor
                                     : (imagePane.maxPanX > 0 || imagePane.maxPanY > 0
                                        ? Qt.OpenHandCursor : Qt.ArrowCursor)
                        property real pressX: 0
                        property real pressY: 0
                        property real startPanX: 0
                        property real startPanY: 0
                        property bool panning: false

                        onPressed: (m) => {
                            pressX = m.x
                            pressY = m.y
                            startPanX = imagePane.panX
                            startPanY = imagePane.panY
                            panning = false
                        }
                        onPositionChanged: (m) => {
                            if (!pressed) {
                                imagePane.hoverIndex = imagePane.lineIndexAt(m.x, m.y)
                                return
                            }
                            const dx = m.x - pressX
                            const dy = m.y - pressY
                            if (!panning && Math.max(Math.abs(dx), Math.abs(dy)) > 3)
                                panning = true
                            if (panning) {
                                imagePane.panX = Math.max(-imagePane.maxPanX,
                                                          Math.min(imagePane.maxPanX,
                                                                   startPanX + dx))
                                imagePane.panY = Math.max(-imagePane.maxPanY,
                                                          Math.min(imagePane.maxPanY,
                                                                   startPanY + dy))
                            }
                        }
                        onReleased: (m) => {
                            if (!panning)
                                resultWindow.clickPicture(m.x, m.y, m.modifiers)
                            panning = false
                        }
                        onExited: imagePane.hoverIndex = -1
                        onWheel: (wheel) => {
                            if (wheel.modifiers & Qt.ControlModifier) {
                                imagePane.zoomTo(imagePane.effectiveZoom
                                                 * (wheel.angleDelta.y > 0 ? 1.25 : 0.8),
                                                 wheel.x, wheel.y)
                            } else if (imagePane.maxPanY > 0 || imagePane.maxPanX > 0) {
                                // A plain wheel scrolls the zoomed picture the
                                // way every viewer does; while it all fits there
                                // is nothing to scroll.
                                imagePane.panY = Math.max(-imagePane.maxPanY,
                                                          Math.min(imagePane.maxPanY,
                                                                   imagePane.panY
                                                                   + wheel.angleDelta.y * 0.6))
                                if (wheel.modifiers & Qt.ShiftModifier)
                                    imagePane.panX = Math.max(-imagePane.maxPanX,
                                                              Math.min(imagePane.maxPanX,
                                                                       imagePane.panX
                                                                       + wheel.angleDelta.y * 0.6))
                            }
                            // The pane owns the wheel: without this the event
                            // keeps bubbling to whatever is behind it.
                            wheel.accepted = true
                        }
                    }
                }

                // The divider. Dragging it stores a real ratio; a double-click
                // (or Backspace when it has focus) goes back to auto. Both
                // orientations share one component: the drag axis follows the
                // pane layout, so a narrow window still gets a usable handle.
                Rectangle {
                    id: splitHandle
                    x: resultWindow.sideBySide ? panes.pictureW : 0
                    y: resultWindow.sideBySide ? 0 : panes.pictureH
                    width: resultWindow.sideBySide ? panes.handleSize : panes.width
                    height: resultWindow.sideBySide ? panes.height : panes.handleSize
                    color: "transparent"

                    Rectangle {
                        anchors.centerIn: parent
                        width: resultWindow.sideBySide ? 2 : Math.round(parent.width * 0.4)
                        height: resultWindow.sideBySide ? Math.round(parent.height * 0.4) : 2
                        radius: 1
                        color: splitHandle.activeFocus || splitMouse.containsMouse
                               ? Theme.accent : Theme.divider
                        Behavior on color { ColorAnimation { duration: Theme.animFast } }
                    }

                    MouseArea {
                        id: splitMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: resultWindow.sideBySide ? Qt.SplitHCursor : Qt.SplitVCursor
                        // Double-click resets to the automatic share, which is
                        // also the way back after a drag went too far.
                        onDoubleClicked: App.settings.ocrResultSplit = 0
                        onPressed: (m) => {
                            splitHandle.forceActiveFocus()
                            splitHandle.dragTo(m)
                        }
                        onPositionChanged: (m) => {
                            if (pressed)
                                splitHandle.dragTo(m)
                        }
                    }

                    function dragTo(m) {
                        const p = mapToItem(panes, m.x, m.y)
                        if (resultWindow.sideBySide)
                            panes.setSplit(p.x / panes.availableW)
                        else
                            panes.setSplit(p.y / panes.availableH)
                    }
                    function step(delta) {
                        panes.setSplit(panes.splitRatio + delta / (resultWindow.sideBySide
                                                                  ? panes.availableW
                                                                  : panes.availableH) * 40)
                    }

                    activeFocusOnTab: true
                    // Dedicated handlers run before everything else, so a held
                    // Ctrl/Shift must hand the chord back instead of stealing it.
                    Keys.onLeftPressed: (e) => {
                        if (resultWindow.sideBySide && UKeys.claim(e))
                            step(-12)
                    }
                    Keys.onRightPressed: (e) => {
                        if (resultWindow.sideBySide && UKeys.claim(e))
                            step(12)
                    }
                    Keys.onUpPressed: (e) => {
                        if (!resultWindow.sideBySide && UKeys.claim(e))
                            step(-12)
                    }
                    Keys.onDownPressed: (e) => {
                        if (!resultWindow.sideBySide && UKeys.claim(e))
                            step(12)
                    }
                    // Backspace has no dedicated handler, so it arrives on the
                    // plain one - unaccepted, which is exactly what a declined
                    // chord needs (a held modifier keeps bubbling).
                    Keys.onPressed: (e) => {
                        if (!UKeys.unmodified(e) || e.key !== Qt.Key_Backspace)
                            return
                        App.settings.ocrResultSplit = 0
                        e.accepted = true
                    }
                    Accessible.role: Accessible.Slider
                    Accessible.name: qsTr("Divider between the picture and the text")
                    Accessible.description: qsTr("Drag it, or use the arrow keys; double-click to reset")
                    Accessible.focusable: true
                    Accessible.onIncreaseAction: step(12)
                    Accessible.onDecreaseAction: step(-12)
                    UFocusRing { hostRadius: 4 }
                }

                Rectangle {
                    id: textPane
                    x: panes.textX
                    y: panes.textY
                    width: panes.textW
                    height: panes.textH
                    color: Theme.secondary
                    radius: Theme.radiusM

                    Flickable {
                        id: textFlick
                        anchors.fill: parent
                        anchors.margins: Theme.spacingM
                        // The chunk progress footer sits on the pane's bottom
                        // edge while a multi-request translation streams; the
                        // text must not run under it.
                        anchors.bottomMargin: App.ocrResult.translating
                                              && App.ocrResult.translationChunkCount > 1
                                              ? Theme.spacingM + 22 : Theme.spacingM
                        contentWidth: width
                        contentHeight: textEdit.contentHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        // Ctrl+wheel over the text changes its size, the way a
                        // browser does. It sits ON the Flickable so it runs
                        // before the Flickable's own wheel handling and only
                        // claims the chord: the plain wheel keeps scrolling.
                        WheelHandler {
                            acceptedModifiers: Qt.ControlModifier
                            onWheel: (wheel) => {
                                const current = App.settings.ocrResultTextSize > 0
                                                ? App.settings.ocrResultTextSize : Theme.fontM
                                App.settings.ocrResultTextSize =
                                    Math.max(9, Math.min(28, current
                                                           + (wheel.angleDelta.y > 0 ? 1 : -1)))
                            }
                        }

                        // One tinted band per chosen line, behind the text.
                        // The pane is one native TextEdit selection, so a
                        // non-adjacent set can only be drawn here; the picture
                        // boxes and these bands share the controller's list.
                        Repeater {
                            model: resultWindow.showTranslation ? [] : App.ocrResult.selectedLines
                            delegate: Rectangle {
                                required property var modelData
                                readonly property var sourceLine: App.ocrResult.lines[modelData]
                                visible: sourceLine !== undefined && textEdit.text.length > 0
                                x: 0
                                width: textFlick.width
                                y: sourceLine
                                   ? textEdit.positionToRectangle(sourceLine.start).y : 0
                                height: {
                                    // Read the text so a layer switch re-measures.
                                    const current = textEdit.text
                                    if (!sourceLine || current.length === 0)
                                        return 0
                                    const first = textEdit.positionToRectangle(sourceLine.start)
                                    const last = textEdit.positionToRectangle(
                                                     sourceLine.start
                                                     + Math.max(0, sourceLine.length - 1))
                                    return Math.max(first.height,
                                                    last.y + last.height - first.y)
                                }
                                color: Theme.alpha(Theme.accent, 0.16)
                                radius: 2
                                z: -1
                            }
                        }

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
                            // 0 follows the theme; Ctrl+wheel stores a real size.
                            font.pixelSize: App.settings.ocrResultTextSize > 0
                                            ? App.settings.ocrResultTextSize : Theme.fontM
                            Accessible.role: Accessible.StaticText
                            Accessible.name: qsTr("Recognized text")
                            Accessible.description: qsTr("Selectable text. Ctrl and the mouse wheel change its size.")
                            // Selecting text in the pane is the partial-text
                            // path: it is a different model from the line-box
                            // set, so a real pane drag clears that set. The
                            // picture's own select() call is guarded.
                            onSelectedTextChanged: {
                                if (!resultWindow.syncingPaneSelection
                                        && textEdit.selectedText.length > 0
                                        && App.ocrResult.selectedLines.length > 0)
                                    App.ocrResult.clearLineSelection()
                            }
                        }
                    }

                    // Per-chunk state, inside the pane it describes: one dot
                    // per request (done, in flight, still pending) plus the
                    // count. Only shown when a page actually needs more than
                    // one request, so a short result keeps its plain pane.
                    Row {
                        id: chunkProgress
                        visible: App.ocrResult.translating
                                 && App.ocrResult.translationChunkCount > 1
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: Theme.spacingM
                        anchors.rightMargin: Theme.spacingM
                        anchors.bottomMargin: Theme.spacingS
                        height: 16
                        spacing: 5

                        Repeater {
                            model: App.ocrResult.translationChunkCount

                            Rectangle {
                                required property int index
                                width: 8
                                height: 8
                                radius: 4
                                anchors.verticalCenter: parent.verticalCenter
                                color: index < App.ocrResult.translationChunksDone
                                       || index === App.ocrResult.translationChunkIndex - 1
                                       ? Theme.accent : Theme.divider
                                opacity: index < App.ocrResult.translationChunksDone ? 1
                                         : index === App.ocrResult.translationChunkIndex - 1
                                           ? 0.75 : 0.4
                            }
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            leftPadding: 4
                            text: qsTr("%1 of %2 chunks")
                                      .arg(App.ocrResult.translationChunksDone)
                                      .arg(App.ocrResult.translationChunkCount)
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontS
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }
                    }
                }
            }
        }
    }

    // First-send privacy notice. It is the only moment the user is told, in
    // words, that the page's text leaves this machine, so it names the host
    // instead of saying "the translation service". The answer is remembered per
    // host by the controller; a different endpoint asks again.
    Popup {
        id: privacyNotice
        parent: Overlay.overlay
        anchors.centerIn: parent
        margins: UFlyout.margin
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        width: Math.min(460, parent ? parent.width - 2 * Theme.spacingXL : 460)
        padding: Theme.spacingXL
        Overlay.modal: Rectangle { color: Theme.modalScrim }
        onClosed: {
            App.ocrResult.cancelPrivacyNotice()
            // The window switched to Translation before asking; with nothing
            // sent, show the page again instead of a blank pane.
            if (App.ocrResult.translation.length === 0 && !App.ocrResult.translating)
                resultWindow.showTranslation = false
        }
        background: Rectangle {
            radius: Theme.radiusL
            color: Theme.surface
            border.width: 1
            border.color: Theme.divider
        }
        contentItem: Column {
            spacing: Theme.spacingM
            Accessible.role: Accessible.Dialog
            Accessible.name: qsTr("Send this text to %1?").arg(App.ocrResult.privacyNoticeHost)
            Accessible.description: qsTr("The text of this screenshot will be sent to a translation service on another computer.")
            Text {
                width: parent.width
                text: qsTr("Send this text to %1?").arg(App.ocrResult.privacyNoticeHost)
                color: Theme.textPrimary
                font.pixelSize: Theme.fontL
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: qsTr("Unisic will send the recognized text from this screenshot to %1, a translation service on another computer, so it leaves this machine. The screenshot itself is not sent.").arg(App.ocrResult.privacyNoticeHost)
                color: Theme.textSecondary
                font.pixelSize: Theme.fontM
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: qsTr("This is asked once for this host. Another endpoint asks again.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontS
                wrapMode: Text.WordWrap
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.spacingS
                UButton {
                    text: qsTr("Don't send")
                    variant: "ghost"
                    compact: true
                    onClicked: {
                        App.ocrResult.cancelPrivacyNotice()
                        privacyNotice.close()
                    }
                }
                UButton {
                    text: qsTr("Translate")
                    variant: "filled"
                    compact: true
                    onClicked: {
                        App.ocrResult.confirmPrivacyNotice()
                        privacyNotice.close()
                    }
                }
            }
        }
    }

    UWindowResizer { active: !App.settings.useSystemDecoration }
}
