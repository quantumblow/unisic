#pragma once
#include <QColor>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVector>
#include <qqmlregistration.h>

#include "OcrWord.h"

#include <algorithm>
#include <memory>

class QTemporaryFile;
class Settings;
class Translator;

// The data behind the OCR result window: the untouched screenshot, the full
// recognized text, and one selectable line box per OCR text line. The window
// keeps the source image visible next to (or below) a real text pane, so the
// mouse can select and copy exactly like a text document, and the line boxes
// on the picture select the matching range in that pane.
//
// It owns a temporary PNG for QML's Image; the file lives until the next
// result or until the app quits. No OCR work happens here.
class OcrResultController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by AppContext")

    Q_PROPERTY(QUrl imageSource READ imageSource NOTIFY resultChanged)
    Q_PROPERTY(QSize imageSize READ imageSize NOTIFY resultChanged)
    Q_PROPERTY(QString text READ text NOTIFY resultChanged)
    // { text, x, y, width, height, start, length } in image pixels; start and
    // length index into `text`, so a click can select the line in the pane.
    Q_PROPERTY(QVariantList lines READ lines NOTIFY resultChanged)
    // OpenAI-compatible translation of `text`; empty until Translate is pressed.
    Q_PROPERTY(QString translation READ translation NOTIFY translationChanged)
    // The same translation split back onto the line boxes, so the picture can
    // show it in place of the original text: { text, x, y, width, height,
    // start, length, sourceStart, sourceLength, background, foreground,
    // fontSize, rtl, overflow }. `start`/`length` index the joined translation
    // and `sourceStart`/`sourceLength` the source line it came from. Empty
    // when the picture has no line geometry (a QR payload) or when the service
    // did not keep the line numbering; the pane then shows the whole text.
    // `fontSize` is the size the C++ fit chose for the box, in IMAGE pixels:
    // the window multiplies it by its render scale instead of running Text.Fit
    // itself, so the saved picture and the screen cannot pick different sizes.
    Q_PROPERTY(QVariantList translatedLines READ translatedLines NOTIFY translationChanged)
    Q_PROPERTY(bool translating READ translating NOTIFY translationChanged)
    Q_PROPERTY(QString translationError READ translationError NOTIFY translationChanged)
    Q_PROPERTY(bool canTranslate READ canTranslate NOTIFY translationChanged)
    Q_PROPERTY(QString translationTarget READ translationTarget NOTIFY translationChanged)
    // True when the last/current translation covers only the lines the user
    // selected in the pane, so the window can say so instead of implying the
    // rest of the page was translated too.
    Q_PROPERTY(bool translationSelectionOnly READ translationSelectionOnly NOTIFY translationChanged)
    // How many recognized lines the last/current translation covers. 0 before
    // the first Translate.
    Q_PROPERTY(int translationLineCount READ translationLineCount NOTIFY translationChanged)
    // How many of those lines were too long for one request and were split at
    // sentence boundaries before being sent. The window tells the reader
    // instead of letting a split look like a truncation.
    Q_PROPERTY(int translationSplitLines READ translationSplitLines NOTIFY translationChanged)
    // Progress while a line-by-line translation runs: how many requests the
    // page needs, which one is in flight (1-based, 0 while idle/finishing) and
    // how many have finished (successfully or not, so the dots never go back).
    Q_PROPERTY(int translationChunkCount READ translationChunkCount NOTIFY translationChanged)
    Q_PROPERTY(int translationChunkIndex READ translationChunkIndex NOTIFY translationChanged)
    Q_PROPERTY(int translationChunksDone READ translationChunksDone NOTIFY translationChanged)
    // Set when Translate is held back for the once-per-host first-send privacy
    // notice: the non-local host the text would go to. Empty otherwise, and
    // never set for 127.0.0.1 / localhost / ::1. The window shows the notice
    // and calls confirmPrivacyNotice() or cancelPrivacyNotice().
    Q_PROPERTY(QString privacyNoticeHost READ privacyNoticeHost NOTIFY privacyNoticeChanged)
    // True when a successful answer is the recognized text itself (a service
    // that echoed the input, often for a language pair it handles badly). The
    // window says so instead of presenting the source as a translation.
    Q_PROPERTY(bool translationUnchanged READ translationUnchanged NOTIFY translationChanged)
    // The lines the user picked on the picture (Ctrl+click toggles, Shift+click
    // ranges, plain click replaces, Ctrl+A all, Esc clears). Indices into
    // `lines`, in document order. This is a controller model on purpose: the
    // pane is one native TextEdit selection and cannot hold a non-adjacent set.
    Q_PROPERTY(QVariantList selectedLines READ selectedLines NOTIFY selectionChanged)
    // True when the last/current translation was the exact selected text
    // (a word or part of a line), not whole lines: the answer is in the pane and
    // must not paint a cover over the line it came from.
    Q_PROPERTY(bool translationPartialText READ translationPartialText NOTIFY translationChanged)
    // A one-line confirmation the window shows in place (Copied, Saved ...,
    // Opened in the editor): the app's toasts live in the MAIN window, which
    // the result window can be covering while the user works in it. Cleared
    // after a few seconds by the controller, so QML keeps no timer of its own.
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusTextChanged)

public:
    // One recognized glyph in the text the pane shows. `start`/`length` index
    // that string, `rect` is the glyph's box in image pixels, `line` indexes
    // `lines()` and `spaceBefore` records the word boundary the engine marked.
    // A word or partial-line selection is resolved against this list, so the
    // overlay can cover exactly the glyphs the reader selected.
    struct Glyph {
        int start = 0;
        int length = 0;
        QRect rect;
        int line = 0;
        bool spaceBefore = false;
    };

    explicit OcrResultController(Settings *settings = nullptr, QObject *parent = nullptr);
    ~OcrResultController() override;

    QUrl imageSource() const { return m_source; }
    QSize imageSize() const { return m_image.size(); }
    QString text() const { return m_text; }
    QVariantList lines() const { return m_lines; }
    // Every recognized glyph, in reading order; empty for a payload without
    // geometry (a QR code). Built in the same pass as `text`, so the two can
    // never describe different strings.
    QVector<Glyph> glyphs() const { return m_glyphs; }
    QImage image() const { return m_image; }
    QString translation() const { return m_translation; }
    QVariantList translatedLines() const { return m_translatedLines; }
    bool translating() const { return m_translating; }
    QString translationError() const { return m_translationError; }
    bool canTranslate() const;
    QString translationTarget() const;
    bool translationSelectionOnly() const { return m_translationSelectionOnly; }
    int translationLineCount() const { return m_translationLineCount; }
    int translationSplitLines() const { return m_translationSplitLines; }
    int translationChunkCount() const { return int(m_chunks.size()); }
    int translationChunkIndex() const
    {
        return m_translating && m_chunkIndex < m_chunks.size() ? m_chunkIndex + 1 : 0;
    }
    int translationChunksDone() const { return std::min(m_chunkIndex, int(m_chunks.size())); }
    bool translationUnchanged() const { return m_translationUnchanged; }
    bool translationPartialText() const { return m_translationPartialText; }
    QVariantList selectedLines() const;
    bool hasLineSelection() const { return !m_selectedLines.isEmpty(); }
    int selectedLineCount() const { return m_selectedLines.size(); }
    QString statusText() const { return m_statusText; }
    bool statusIsError() const { return m_statusIsError; }
    Q_INVOKABLE void setStatusText(const QString &text, bool error = false);

    // Translates the current text with the OpenAI-compatible endpoint from
    // Settings. One request at a time; a new OCR result cancels it.
    Q_INVOKABLE void translate();
    // Translates only the recognized lines whose [start, start+length) range
    // in `text` overlaps the selection. A selection covering no line (or an
    // empty one) falls back to the whole text, like translate(). A selection
    // that is exactly whole lines keeps the numbered request and the picture
    // covers; anything shorter (a word, part of a line) translates that exact
    // text with its line as context and only fills the pane (T4.5.2).
    Q_INVOKABLE void translateRange(int start, int length);
    // Multi-select (T4.5.1): the chosen whole lines, in document order.
    Q_INVOKABLE void selectLine(int index);
    Q_INVOKABLE void toggleLine(int index);
    Q_INVOKABLE void extendLineSelection(int index);
    Q_INVOKABLE void selectAllLines();
    Q_INVOKABLE void clearLineSelection();
    // The chosen lines' texts joined with one newline, in document order. Empty
    // when the set is empty; the window falls back to the native pane selection.
    Q_INVOKABLE QString selectionText() const;
    // Translates the chosen lines through the numbered path (and covers). Does
    // nothing when the set is empty; translate() then handles the pane.
    Q_INVOKABLE void translateSelectedLines();
    // True when [start, start+length) is exactly the union of the whole lines it
    // touches, the condition for the numbered request and the covers.
    static bool rangeCoversWholeLines(const QVariantList &lines, int start, int length);
    Q_INVOKABLE bool rangeCoversWholeLines(int start, int length) const;
    Q_INVOKABLE void cancelTranslation();
    // The first-send privacy notice: Translate stops before any request when
    // the endpoint is not localhost and this host has not been approved yet.
    // Confirm remembers the host and runs the held request; cancel drops it.
    QString privacyNoticeHost() const { return m_privacyNoticeHost; }
    Q_INVOKABLE void confirmPrivacyNotice();
    Q_INVOKABLE void cancelPrivacyNotice();
    // Developer/smoke path: lay `translated` (one entry per line box, exactly
    // what a service answer produces) onto the picture without touching the
    // network, so the window's Save/Copy image, Edit in editor, zoom and split
    // can be exercised by hand. Cancels any translation in flight.
    void setTranslationForPreview(const QStringList &translated);

    // The picture at source resolution: the untouched capture for the Original
    // layer, or the covers with their fitted text for the Translation layer.
    // Saving, copying and handing the picture to the editor all go through here,
    // so they can never pick up the window's scaled-down copy instead.
    QImage compositedImage(bool translated) const;

    // The line under a point in IMAGE coordinates (the window converts its own
    // scaled, panned point into these, so the test is exact at every zoom), or
    // -1. `translated` tests the padded covers, which is what the Translation
    // layer shows.
    Q_INVOKABLE int lineAt(const QPointF &imagePoint, bool translated) const;

    // The font fitting both the composite and the window use: the largest
    // integer pixel size, from `maxPixelSize` down to kMinFitPixelSize, at
    // which `text` wrapped at the box width still fits its height. `fits` comes
    // back false when even the floor does not, which is what the overflow
    // marker reports instead of silently clipping.
    static int fittedPixelSize(const QString &text, const QSize &box, int maxPixelSize,
                               bool *fits = nullptr);
    static constexpr int kMinFitPixelSize = 5;
    // True when the line reads right to left (first strong character is
    // Arabic/Hebrew/...): the cover aligns its text to the right edge like the
    // pane does, instead of left-aligning an RTL run.
    static bool lineIsRtl(const QString &text);
    // Index of the entry whose rect holds `imagePoint`, or -1.
    static int lineIndexAt(const QVariantList &lines, const QPointF &imagePoint);
    // Indices of the line boxes whose [start, start+length) range overlaps the
    // same range of the source text. Empty when the selection touches no box
    // (a QR payload or a selection of only the newline between two lines).
    static QVector<int> selectedLineIndexes(const QVariantList &lines, int start, int length);
    // Draws the covers onto a copy of `image` at its own resolution.
    static QImage composeOverlay(const QImage &image, const QVariantList &translatedLines);

    // `words` empty is legal: a QR/barcode payload has text but no geometry.
    // The text is rebuilt from `words` when there are any, so line offsets and
    // the selectable ranges cannot drift from the picture.
    void setResult(const QImage &image, const QVector<OcrWord> &words, const QString &text);

    // Colours for covering one line box: the background is the mode of the box
    // interior (the page between the glyphs) and the text colour the mode of
    // the pixels that differ from it, so the cover blends into the page - and a
    // line on a coloured banner takes the banner colour. Falls back to black or
    // white when the sampled pair is unreadable together.
    struct LineColors {
        QColor background;
        QColor foreground;
    };
    static LineColors sampleLineColors(const QImage &image, const QRect &rect);
    // Lays `translated` (one entry per line box) onto `lines`: the joined text
    // goes to `text`, and every entry carries the box geometry plus its
    // [start, length) range in that text, so clicking a translated line on the
    // picture selects exactly its translation in the pane.
    static QVariantList translatedLineMap(const QVariantList &lines, const QStringList &translated,
                                          const QImage &image, QString *text);
    // Comparing with whitespace squashed: a service that echoes the input often
    // re-wraps or re-spaces it, and that is still "unchanged" to the reader.
    static bool translationLooksUnchanged(const QString &source, const QString &translation);
    // A page can be hundreds of lines; one request per this many (and per
    // Translator::kRequestTokenBudget) keeps the prompt inside a small model's
    // context and still leaves every answer mappable.
    static QVector<QStringList> chunkLines(const QStringList &lines, int perRequest = 40);

signals:
    void resultChanged();
    void translationChanged();
    void statusTextChanged();
    void selectionChanged();
    void privacyNoticeChanged();
    // A new notice is waiting; the window opens its dialog.
    void privacyNoticeRequested();
    // A new translation finished; the window switches to the Translation view.
    void translationReady();

private:
    QImage m_image;
    QUrl m_source;
    QString m_text;
    QVariantList m_lines;
    QVector<Glyph> m_glyphs;
    // Parallel to m_lines / m_translatedLines: the hit-test rects, so a mouse
    // move does not convert a whole QVariantList of maps.
    QVector<QRect> m_lineRects;
    QVector<QRect> m_coverRects;
    std::unique_ptr<QTemporaryFile> m_file;
    quint64 m_revision = 0;
    Settings *m_settings = nullptr;
    Translator *m_translator = nullptr;
    QString m_translation;
    QVariantList m_translatedLines;
    bool m_translationUnchanged = false;
    QString m_translationError;
    bool m_translating = false;
    // The line boxes the current translation covers (all of m_lines, or the
    // selection) and the text they join to. Every request and every map uses
    // these instead of m_lines/m_text, so a selection translation can never
    // place a cover on a line it did not send.
    QVariantList m_translateLines;
    // m_translateLines[i] came from this line of m_lines. The whole text maps
    // to itself; a selection maps to the source lines the reader counts, so a
    // failed-chunk message can name them.
    QVector<int> m_translateLineSource;
    QString m_sourceText;
    bool m_translationSelectionOnly = false;
    int m_translationLineCount = 0;
    int m_translationSplitLines = 0;
    QString m_statusText;
    bool m_statusIsError = false;
    QTimer m_statusTimer;
    // Multi-select over the line boxes, and the line a Shift+click extends
    // from. Both reset with a new result.
    QVector<int> m_selectedLines;
    int m_selectionAnchor = -1;
    bool m_translationPartialText = false;
    QString m_partialContext;
    // What the held first-send notice should run once the user answers.
    enum class PendingTranslation { None, Range, SelectedLines };
    PendingTranslation m_privacyPending = PendingTranslation::None;
    // One line can be split into several request items (T3.3), so a chunk is a
    // list of item indices rather than a list of lines. `m_itemTranslations`
    // is parallel to m_items; assembling per line is what puts the parts back
    // together before the cover map is built.
    struct TranslationItem {
        int line = 0;
        QString text;
    };
    QVector<TranslationItem> m_items;
    QVector<QVector<int>> m_chunks;
    QVector<QString> m_itemTranslations;
    // A chunk gets one automatic retry on a transport error or a 5xx; a chunk
    // that still fails is recorded instead of dropping the whole translation.
    QVector<bool> m_chunkRetried;
    QVector<int> m_failedLines;
    QString m_lastChunkError;
    bool m_wholeTextRetried = false;
    // The in-flight answer and the text the pane shows while it streams. The
    // pane refresh is throttled: a token per event would re-lay out the whole
    // TextEdit dozens of times a second.
    QString m_streamText;
    QTimer m_streamTimer;
    // A line-by-line translation runs one numbered request per chunk; the
    // generation bumps on cancel or a new result and makes every late callback
    // a no-op.
    quint64 m_translationSeq = 0;
    int m_chunkIndex = 0;

    // The once-per-host first-send notice (T4.3): the held request and the host
    // it would go to, empty when nothing is waiting.
    QString m_privacyNoticeHost;
    int m_privacyStart = -1;
    int m_privacyLength = 0;
    void clearPrivacyNotice();
    // True when the text would leave the machine and this host has not been
    // approved yet; the request is held and the window asked.
    bool holdForPrivacy(PendingTranslation kind, int start, int length);
    void requestNextLineChunk(quint64 seq);
    void requestWholeText(quint64 seq);
    // The numbered path: `selected` are the source lines, empty means all.
    void startLineTranslation(const QVector<int> &selected, bool selectionOnly);
    // T4.5.2: the exact selected text, with the touched lines as context, pane
    // only.
    void startPartialTranslation(const QString &selection, const QString &context);
    void requestPartialText(quint64 seq);
    void setSelectedLines(const QVector<int> &lines);
    void finishLineTranslation();
    QString failedLinesSummary() const;
    void startTranslation(int start, int length);
    void failTranslation(quint64 seq, const QString &error);
    void refreshStreamingPane();
    QStringList assembledLineTranslations() const;
    QString streamingPaneText() const;
    // One place that assigns m_translatedLines, so the cover hit-test rects
    // can never describe a different list than the one the window draws.
    void setTranslatedLines(const QVariantList &lines);
};
