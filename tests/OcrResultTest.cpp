#include <QtTest>
#include <cmath>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTextLayout>
#include <QTextOption>
#include <QVariantList>
#include <QVariantMap>

#include "MockTranslationEndpoint.h"
#include "Settings.h"
#include "ocr/OcrResultController.h"

namespace {

// The fitting criterion checked independently of the controller: build the same
// wrap QML's Text does and see whether the wrapped block fits the box height.
bool textFits(const QString &text, const QSize &box, int pixelSize, bool rtl)
{
    if (text.isEmpty() || box.width() <= 0 || box.height() <= 0)
        return false;
    QFont font = QGuiApplication::font();
    font.setPixelSize(pixelSize);
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setAlignment(rtl ? Qt::AlignRight : Qt::AlignLeft);
    layout.setTextOption(option);
    layout.beginLayout();
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(box.width());
    }
    layout.endLayout();
    return layout.lineCount() > 0
           && layout.lineCount() * QFontMetrics(font).lineSpacing() <= box.height();
}

QVariantMap lineEntry(const QString &text, const QRect &rect)
{
    return QVariantMap{{QStringLiteral("text"), text},
                       {QStringLiteral("x"), rect.x()},
                       {QStringLiteral("y"), rect.y()},
                       {QStringLiteral("width"), rect.width()},
                       {QStringLiteral("height"), rect.height()},
                       {QStringLiteral("start"), 0},
                       {QStringLiteral("length"), text.size()}};
}

QRect entryRect(const QVariantMap &entry)
{
    return QRect(entry.value(QStringLiteral("x")).toInt(),
                 entry.value(QStringLiteral("y")).toInt(),
                 entry.value(QStringLiteral("width")).toInt(),
                 entry.value(QStringLiteral("height")).toInt());
}

// One OcrWord per character, which is what both OCR engines emit: each glyph
// owns its rect and the first glyph of a word carries spaceBefore. The width is
// distributed evenly, so a selection resolves to a realistic character range
// instead of the whole line.
QVector<OcrWord> glyphWordsFor(const QStringList &lines, const QVector<QRect> &boxes)
{
    QVector<OcrWord> words;
    for (int line = 0; line < lines.size() && line < boxes.size(); ++line) {
        const QString &text = lines.at(line);
        const QRect box = boxes.at(line);
        const int step = qMax(1, box.width() / qMax(1, text.size()));
        int x = box.x();
        bool firstGlyph = true;
        for (const QString &token : text.split(QLatin1Char(' '))) {
            bool firstOfWord = true;
            for (const QChar c : token) {
                OcrWord word;
                word.line = line;
                word.text = QString(c);
                word.rect = QRect(x, box.y(), step, box.height());
                word.spaceBefore = firstOfWord && !firstGlyph;
                firstGlyph = false;
                firstOfWord = false;
                words.append(word);
                x += step;
            }
            x += step; // the space between two words is not a glyph
        }
    }
    return words;
}

// A capture with four text lines, each with its own glyph stroke, plus the
// translation that goes on them. The same four-line fixture the golden
// composite test walks over.
struct FourLineFixture {
    QImage image;
    QVariantList lines;
    QVariantList overlay;
    QString joined;
    QVector<QRect> covers;
};

FourLineFixture fourLineFixture(int width, int height, int firstY, int lineHeight, int step)
{
    FourLineFixture f;
    f.image = QImage(width, height, QImage::Format_RGB32);
    f.image.fill(QColor(250, 250, 245));
    QPainter painter(&f.image);
    const QStringList texts{QStringLiteral("Hello"), QStringLiteral("Second line"),
                            QStringLiteral("Third line here"), QStringLiteral("Fourth")};
    const QStringList translated{QStringLiteral("Cześć"), QStringLiteral("Druga linia"),
                                 QStringLiteral("Trzecia linia"), QStringLiteral("Czwarta")};
    const int textWidth = width / 2;
    for (int i = 0; i < 4; ++i) {
        const QRect box(width / 20, firstY + i * step, textWidth, lineHeight);
        painter.fillRect(QRect(box.x() + 2, box.y() + box.height() / 3, box.width() / 2, 3),
                         QColor(20, 20, 20));
        f.lines.append(lineEntry(texts.at(i), box));
    }
    painter.end();
    f.overlay = OcrResultController::translatedLineMap(f.lines, translated, f.image, &f.joined);
    for (const QVariant &value : f.overlay)
        f.covers.append(entryRect(value.toMap()));
    return f;
}

} // namespace

// The result window's line ranges are the only thing that makes a click on the
// picture select the right text. Pin the text/offset/rect contract without any
// OCR model or display server.
class OcrResultTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void rangesMatchTheText();
    void glyphRangesIndexTheText();
    void payloadWithoutGeometry();
    void selectionMapsToWholeLines();
    void streamingFillsThePaneAndCancelStopsIt();
    void privacyNoticeAsksOncePerRemoteHost();
    void multiSelectionOrdersAndMerges();
    void wholeLineMultiSelectionPaintsCovers();
    void partialTextStaysInThePane();
    void partialAnswerCoversOnlyItsGlyphs();
    void longPartialAnswerShowsACallout();
    void partialAcrossLinesShowsOneCallout();
    void partialCalloutStreamsWithTheAnswer();
    void screenOnlyCoverStaysOutOfTheComposite();
    void longLineIsSplitAndRejoined();
    void retriesAChunkOnceThenSucceeds();
    void partialTranslationNamesFailedLines();
    void translatedLinesIndexThePane();
    void lineColorsFollowThePicture();
    void longPagesSplitIntoRequests();
    void spotsAnUnchangedAnswer();
    void overlayFontSizeFitsTheBox();
    void cjkCoverSetsItselfSlightlySmaller();
    void compositeKeepsSourceResolution();
    void compositeDrawsOnlyInsideCovers();
    void clickFindsTheLineAtEveryScale();
    void coverStopsAtTheNeighbourColumn();
    void rtlTranslationAlignsRight();
};

void OcrResultTest::initTestCase()
{
    // Settings is what the controller asks for the endpoint; keep its writes
    // inside the QtTest sandbox.
    QStandardPaths::setTestModeEnabled(true);
}

void OcrResultTest::rangesMatchTheText()
{
    OcrResultController result;
    QSignalSpy changed(&result, &OcrResultController::resultChanged);

    QVector<OcrWord> words;
    const auto add = [&words](int line, const QString &text, const QRect &rect, bool space) {
        OcrWord word;
        word.line = line;
        word.text = text;
        word.rect = rect;
        word.spaceBefore = space;
        words.append(word);
    };
    add(0, QStringLiteral("H"), QRect(0, 0, 10, 10), false);
    add(0, QStringLiteral("i"), QRect(10, 0, 10, 10), false);
    add(1, QStringLiteral("Y"), QRect(0, 20, 10, 10), false);
    add(1, QStringLiteral("o"), QRect(10, 20, 10, 10), true);

    // The passed text is deliberately wrong: with glyphs present the controller
    // must rebuild it, or the selectable ranges would describe a different string.
    result.setResult(QImage(40, 40, QImage::Format_RGB32), words, QStringLiteral("ignored"));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(result.text(), QStringLiteral("Hi\nY o"));

    const QVariantList lines = result.lines();
    QCOMPARE(lines.size(), 2);
    const QVariantMap first = lines.at(0).toMap();
    const QVariantMap second = lines.at(1).toMap();
    QCOMPARE(first.value(QStringLiteral("start")).toInt(), 0);
    QCOMPARE(first.value(QStringLiteral("length")).toInt(), 2);
    QCOMPARE(result.text().mid(0, 2), QStringLiteral("Hi"));
    QCOMPARE(second.value(QStringLiteral("start")).toInt(), 3);
    QCOMPARE(second.value(QStringLiteral("length")).toInt(), 3);
    QCOMPARE(result.text().mid(3, 3), QStringLiteral("Y o"));
    QCOMPARE(second.value(QStringLiteral("x")).toInt(), 0);
    QCOMPARE(second.value(QStringLiteral("y")).toInt(), 20);
    QCOMPARE(second.value(QStringLiteral("width")).toInt(), 20);
    QCOMPARE(second.value(QStringLiteral("height")).toInt(), 10);
    QVERIFY(!result.imageSource().isEmpty());
    QCOMPARE(result.imageSize(), QSize(40, 40));
}

// M4.6: a word or partial-line overlay is resolved against the glyph list, so
// every glyph's [start, length) range has to be exactly its own glyph in the
// string the pane shows - including the spaces the engine marked and the
// newline between two lines. Everything else (a cover, a callout anchor) is
// read off this list, so a drift here would put a word's translation on its
// neighbour.
void OcrResultTest::glyphRangesIndexTheText()
{
    struct Expected {
        int line;
        const char *text;
        QRect rect;
        bool space;
    };
    const QVector<Expected> expected{
        {0, "H", QRect(0, 0, 10, 10), false},
        {0, "i", QRect(10, 0, 10, 10), false},
        {0, "t", QRect(24, 0, 10, 10), true},
        {0, "h", QRect(34, 0, 10, 10), false},
        {0, "e", QRect(44, 0, 10, 10), false},
        {1, "Y", QRect(0, 20, 10, 10), false},
        {1, "o", QRect(10, 20, 10, 10), true},
    };
    QVector<OcrWord> words;
    for (const Expected &e : expected) {
        OcrWord word;
        word.line = e.line;
        word.text = QString::fromLatin1(e.text);
        word.rect = e.rect;
        word.spaceBefore = e.space;
        words.append(word);
    }

    OcrResultController result;
    const QImage image(40, 40, QImage::Format_RGB32);
    // The passed text is deliberately different: with glyphs present the
    // controller rebuilds it, and the ranges index that rebuilt string.
    result.setResult(image, words, QStringLiteral("ignored"));
    QCOMPARE(result.text(), QStringLiteral("Hi the\nY o"));

    const QVector<OcrResultController::Glyph> glyphs = result.glyphs();
    QCOMPARE(glyphs.size(), expected.size());
    const QVariantList lines = result.lines();
    QCOMPARE(lines.size(), 2);
    int previousEnd = 0;
    for (int i = 0; i < glyphs.size(); ++i) {
        const OcrResultController::Glyph &glyph = glyphs.at(i);
        const Expected &want = expected.at(i);
        QVERIFY(glyph.start >= previousEnd);
        QVERIFY(glyph.length > 0);
        QVERIFY(glyph.start + glyph.length <= result.text().size());
        QCOMPARE(result.text().mid(glyph.start, glyph.length),
                 QString::fromLatin1(want.text));
        QCOMPARE(glyph.rect, want.rect);
        QCOMPARE(glyph.line, want.line);
        QCOMPARE(glyph.spaceBefore, want.space);
        // The space the engine marked really is the character before it.
        if (glyph.spaceBefore)
            QCOMPARE(result.text().at(glyph.start - 1), QLatin1Char(' '));
        // Its line's range contains it, so a selection can map a glyph to a
        // whole line without a second pass.
        const QVariantMap line = lines.at(want.line).toMap();
        const int lineStart = line.value(QStringLiteral("start")).toInt();
        const int lineLength = line.value(QStringLiteral("length")).toInt();
        QVERIFY(glyph.start >= lineStart);
        QVERIFY(glyph.start + glyph.length <= lineStart + lineLength);
        QVERIFY(line.contains(QStringLiteral("x")));
        previousEnd = glyph.start + glyph.length;
    }

    // A payload without geometry has no glyphs either; nothing may index a
    // string that has no boxes behind it.
    OcrResultController payload;
    payload.setResult(image, {}, QStringLiteral("https://example.test/abc"));
    QVERIFY(payload.glyphs().isEmpty());
}

void OcrResultTest::payloadWithoutGeometry()
{
    OcrResultController result;
    const QString payload = QStringLiteral("https://example.test/abc");
    result.setResult(QImage(20, 20, QImage::Format_RGB32), {}, payload);
    QCOMPARE(result.text(), payload);
    QVERIFY(result.lines().isEmpty());
    QVERIFY(!result.imageSource().isEmpty());
}

// The overlay's click-to-select behaves exactly like the original boxes, so the
// per-line translation ranges have to index the string the pane shows, and the
// cover has to be filled with the page colour of that line.
// "Translate the selection" sends whole recognized lines, so the selection has
// to be widened to the boxes it touches - never narrowed to the characters the
// pointer crossed, and never spilled onto the line after it. The reverse map
// (sourceStart/sourceLength on a cover) is what lets a selection made on the
// Translation layer find those same source lines again.
void OcrResultTest::selectionMapsToWholeLines()
{
    const QVariantList lines{
        QVariantMap{{QStringLiteral("text"), QStringLiteral("One")},
                    {QStringLiteral("start"), 0}, {QStringLiteral("length"), 3},
                    {QStringLiteral("x"), 10}, {QStringLiteral("y"), 10},
                    {QStringLiteral("width"), 60}, {QStringLiteral("height"), 16}},
        QVariantMap{{QStringLiteral("text"), QStringLiteral("Two")},
                    {QStringLiteral("start"), 4}, {QStringLiteral("length"), 3},
                    {QStringLiteral("x"), 10}, {QStringLiteral("y"), 40},
                    {QStringLiteral("width"), 60}, {QStringLiteral("height"), 16}},
        QVariantMap{{QStringLiteral("text"), QStringLiteral("Three")},
                    {QStringLiteral("start"), 8}, {QStringLiteral("length"), 5},
                    {QStringLiteral("x"), 10}, {QStringLiteral("y"), 70},
                    {QStringLiteral("width"), 70}, {QStringLiteral("height"), 16}},
    };
    QCOMPARE(OcrResultController::selectedLineIndexes(lines, 0, 1), QVector<int>{0});
    QCOMPARE(OcrResultController::selectedLineIndexes(lines, 2, 3), (QVector<int>{0, 1}));
    // Only the newline between two lines: no box, so the caller sends it all.
    QVERIFY(OcrResultController::selectedLineIndexes(lines, 3, 1).isEmpty());
    QCOMPARE(OcrResultController::selectedLineIndexes(lines, 8, 5), QVector<int>{2});
    QVERIFY(OcrResultController::selectedLineIndexes(lines, -1, 0).isEmpty());
    QVERIFY(OcrResultController::selectedLineIndexes(lines, 0, 0).isEmpty());

    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    QString joined;
    const QVariantList map = OcrResultController::translatedLineMap(
        lines, {QStringLiteral("Jeden"), QStringLiteral("Dwa"), QStringLiteral("Trzy")},
        image, &joined);
    QCOMPARE(map.size(), 3);
    QCOMPARE(map.at(0).toMap().value(QStringLiteral("sourceStart")).toInt(), 0);
    QCOMPARE(map.at(1).toMap().value(QStringLiteral("sourceStart")).toInt(), 4);
    QCOMPARE(map.at(1).toMap().value(QStringLiteral("sourceLength")).toInt(), 3);
    QCOMPARE(map.at(2).toMap().value(QStringLiteral("sourceStart")).toInt(), 8);
}

// Streaming goes through the controller, not just the parser: the pane has to
// fill while the request is still open, and Cancel has to drop the partial
// answer instead of leaving half a page behind.
void OcrResultTest::streamingFillsThePaneAndCancelStopsIt()
{
    MockTranslationEndpoint server(MockTranslationEndpoint::SlowSse);
    QVERIFY(server.start());
    Settings settings;
    settings.setTranslationEndpoint(server.base());
    settings.setTranslationModel(QStringLiteral("mock"));
    settings.setTranslationTarget(QStringLiteral("Polish"));

    OcrResultController result(&settings);
    QVector<OcrWord> words;
    const QStringList texts{QStringLiteral("One"), QStringLiteral("Two"),
                            QStringLiteral("Three")};
    for (int i = 0; i < texts.size(); ++i) {
        OcrWord word;
        word.line = i;
        word.text = texts.at(i);
        word.rect = QRect(10, 10 + i * 30, 100, 20);
        words.append(word);
    }
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    result.setResult(image, words, QString());
    QCOMPARE(result.translationLineCount(), 0);

    result.translate();
    QVERIFY(result.translating());
    QCOMPARE(result.translationLineCount(), 3);
    QCOMPARE(result.translationChunkCount(), 1);
    QCOMPARE(result.translationChunkIndex(), 1);
    QVERIFY(!result.translationSelectionOnly());
    QVERIFY(result.translation().isEmpty());

    // The first SSE frame is already on screen while the answer is still open.
    QTRY_VERIFY_WITH_TIMEOUT(result.translating() && !result.translation().isEmpty(),
                             3000);
    result.cancelTranslation();
    QVERIFY(!result.translating());
    QVERIFY(result.translation().isEmpty());
    QVERIFY(result.translatedLines().isEmpty());
    // No late delta or done callback may revive the cancelled answer.
    QTest::qWait(300);
    QVERIFY(result.translation().isEmpty());
    QVERIFY(!result.translating());
}

// T4.3: the first request to a non-local host waits for a yes and is held back
// until then; local endpoints never ask; the yes is remembered per host, so a
// different endpoint asks again.
void OcrResultTest::privacyNoticeAsksOncePerRemoteHost()
{
    Settings settings;
    // QtTest's config outlives the run, and confirming the notice writes the
    // approved host: start from a clean slate or the second run asks nothing.
    settings.setTranslationPrivacyHost(QString());
    settings.setTranslationEndpoint(QStringLiteral("https://api.example.test/v1"));
    settings.setTranslationModel(QStringLiteral("mock"));

    OcrResultController result(&settings);
    OcrWord word;
    word.line = 0;
    word.text = QStringLiteral("Hello");
    word.rect = QRect(10, 10, 80, 20);
    result.setResult(QImage(120, 60, QImage::Format_RGB32), {word}, QString());

    QSignalSpy requested(&result, &OcrResultController::privacyNoticeRequested);
    QSignalSpy changed(&result, &OcrResultController::privacyNoticeChanged);

    result.translate();
    QCOMPARE(requested.count(), 1);
    QCOMPARE(result.privacyNoticeHost(), QStringLiteral("api.example.test"));
    QVERIFY(!result.translating());
    QVERIFY(result.translation().isEmpty());
    QVERIFY(settings.translationPrivacyHost().isEmpty());

    // A no sends nothing either.
    result.cancelPrivacyNotice();
    QVERIFY(result.privacyNoticeHost().isEmpty());
    QCOMPARE(changed.count(), 2); // set, cleared
    result.translate();
    QCOMPARE(requested.count(), 2);

    // A yes remembers the host and starts the request the notice held back.
    result.confirmPrivacyNotice();
    QCOMPARE(settings.translationPrivacyHost(), QStringLiteral("api.example.test"));
    QVERIFY(result.privacyNoticeHost().isEmpty());
    QVERIFY(result.translating());
    result.cancelTranslation();

    // The same host never asks again...
    result.translate();
    QCOMPARE(requested.count(), 2);
    QVERIFY(result.translating());
    result.cancelTranslation();

    // ...but another endpoint does, because the text would go elsewhere.
    settings.setTranslationEndpoint(QStringLiteral("https://other.example.test/v1"));
    result.translate();
    QCOMPARE(requested.count(), 3);
    QCOMPARE(result.privacyNoticeHost(), QStringLiteral("other.example.test"));
    result.cancelPrivacyNotice();

    // A local service never asks: that is the plan's 127.0.0.1/localhost/::1
    // exemption, checked end to end through the controller.
    MockTranslationEndpoint server(MockTranslationEndpoint::Sse);
    QVERIFY(server.start());
    Settings local;
    local.setTranslationEndpoint(server.base());
    local.setTranslationModel(QStringLiteral("mock"));
    OcrResultController localResult(&local);
    localResult.setResult(QImage(120, 60, QImage::Format_RGB32), {word}, QString());
    QSignalSpy localRequested(&localResult, &OcrResultController::privacyNoticeRequested);
    localResult.translate();
    QVERIFY(localResult.translating());
    QCOMPARE(localRequested.count(), 0);
    QVERIFY(localResult.privacyNoticeHost().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!localResult.translating(), 5000);
    QCOMPARE(localResult.translation(), QStringLiteral("Hello world"));
}

// T4.5.1: the line-box set is a controller model: plain click replaces,
// Ctrl+click toggles, Shift+click extends from the anchor, Ctrl+A takes all,
// Esc (clearLineSelection) empties it, and Copy order is document order.
void OcrResultTest::multiSelectionOrdersAndMerges()
{
    OcrResultController result;
    QVector<OcrWord> words;
    const QStringList texts{QStringLiteral("One"), QStringLiteral("Two"),
                            QStringLiteral("Three"), QStringLiteral("Four")};
    for (int i = 0; i < texts.size(); ++i) {
        OcrWord word;
        word.line = i;
        word.text = texts.at(i);
        word.rect = QRect(10, 10 + i * 30, 100, 20);
        words.append(word);
    }
    result.setResult(QImage(160, 160, QImage::Format_RGB32), words, QString());
    QCOMPARE(result.lines().size(), 4);

    result.selectLine(2);
    QCOMPARE(result.selectedLines(), QVariantList{2});
    result.toggleLine(0);
    QCOMPARE(result.selectedLines(), (QVariantList{0, 2}));
    QCOMPARE(result.selectionText(), QStringLiteral("One\nThree"));
    result.toggleLine(2);
    QCOMPARE(result.selectedLines(), QVariantList{0});

    // Shift extends from the anchor (the last toggled line).
    result.selectLine(0);
    result.extendLineSelection(2);
    QCOMPARE(result.selectedLines(), (QVariantList{0, 1, 2}));
    QCOMPARE(result.selectionText(), QStringLiteral("One\nTwo\nThree"));

    result.selectAllLines();
    QCOMPARE(result.selectedLines(), (QVariantList{0, 1, 2, 3}));
    result.clearLineSelection();
    QVERIFY(result.selectedLines().isEmpty());
    QVERIFY(result.selectionText().isEmpty());

    // An invalid index never enters the set.
    result.selectLine(9);
    QVERIFY(result.selectedLines().isEmpty());

    // Whole-line classification: the exact union is whole, one character short
    // or a partial start is not.
    const QVariantList lines = result.lines();
    QVERIFY(OcrResultController::rangeCoversWholeLines(lines, 0, 3));
    // The exact text of lines 1..2 is "Two\nThree" = 9 characters.
    QVERIFY(OcrResultController::rangeCoversWholeLines(lines, 4, 9));
    QVERIFY(!OcrResultController::rangeCoversWholeLines(lines, 5, 8));
    QVERIFY(!OcrResultController::rangeCoversWholeLines(lines, 0, 2));
    QVERIFY(!OcrResultController::rangeCoversWholeLines(lines, -1, 0));
}

// T4.5.1: whole-line multi-select still uses the numbered request and paints
// only the chosen lines' covers; the lines between them are not sent.
void OcrResultTest::wholeLineMultiSelectionPaintsCovers()
{
    MockTranslationEndpoint server(MockTranslationEndpoint::NumberedSse);
    QVERIFY(server.start());
    Settings settings;
    settings.setTranslationEndpoint(server.base());
    settings.setTranslationModel(QStringLiteral("mock"));
    settings.setTranslationTarget(QStringLiteral("Polish"));

    OcrResultController result(&settings);
    QVector<OcrWord> words;
    const QStringList texts{QStringLiteral("One"), QStringLiteral("Two"),
                            QStringLiteral("Three")};
    for (int i = 0; i < texts.size(); ++i) {
        OcrWord word;
        word.line = i;
        word.text = texts.at(i);
        word.rect = QRect(10, 10 + i * 30, 120, 20);
        words.append(word);
    }
    QImage image(180, 120, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    result.setResult(image, words, QString());

    result.selectLine(0);
    result.toggleLine(2);
    QCOMPARE(result.selectedLines(), (QVariantList{0, 2}));
    result.translateSelectedLines();
    QTRY_VERIFY_WITH_TIMEOUT(!result.translating(), 5000);
    QCOMPARE(result.translationError(), QString());
    QVERIFY(result.translationSelectionOnly());
    QVERIFY(!result.translationPartialText());
    QCOMPARE(result.translatedLines().size(), 2);
    QCOMPARE(result.translation(), QStringLiteral("Translated part 1\nTranslated part 2"));
    // Only the two chosen lines went out; the one between them did not.
    QCOMPARE(server.bodies.size(), 1);
    const QByteArray &request = server.bodies.first();
    QVERIFY(request.contains("1. One"));
    QVERIFY(request.contains("2. Three"));
    QVERIFY(!request.contains("Two"));
    // The covers keep the source lines they came from.
    const QVariantList covers = result.translatedLines();
    QCOMPARE(covers.at(0).toMap().value(QStringLiteral("sourceStart")).toInt(), 0);
    QCOMPARE(covers.at(1).toMap().value(QStringLiteral("sourceStart")).toInt(), 8);
}

// T4.5.2 + M4.6: a word or part of a line sends exactly that text, with the
// line as context. The answer now shows on the picture: a fitted answer covers
// the selected glyphs (screen only), and a selection crossing a line boundary
// gets one callout because a cover cannot be split across lines.
void OcrResultTest::partialTextStaysInThePane()
{
    MockTranslationEndpoint server(MockTranslationEndpoint::Sse);
    QVERIFY(server.start());
    Settings settings;
    settings.setTranslationEndpoint(server.base());
    settings.setTranslationModel(QStringLiteral("mock"));
    settings.setTranslationTarget(QStringLiteral("Polish"));

    OcrResultController result(&settings);
    QVector<OcrWord> words;
    const QStringList texts{QStringLiteral("One two three"), QStringLiteral("Second line")};
    for (int i = 0; i < texts.size(); ++i) {
        OcrWord word;
        word.line = i;
        word.text = texts.at(i);
        word.rect = QRect(10, 10 + i * 30, 150, 20);
        words.append(word);
    }
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    result.setResult(image, words, QString());

    const int start = result.text().indexOf(QStringLiteral("two"));
    QVERIFY(start >= 0);
    result.translateRange(start, 3);
    QCOMPARE(result.translationLineCount(), 0);
    QVERIFY(result.translationPartialText());
    QVERIFY(!result.translationSelectionOnly());
    QVERIFY(result.translating());
    QTRY_VERIFY_WITH_TIMEOUT(!result.translating(), 5000);
    QCOMPARE(result.translationError(), QString());
    QCOMPARE(result.translation(), QStringLiteral("Hello world"));
    // "Hello world" fits the selected line box, so it goes over the glyphs, as
    // one screen-only cover - never a whole-line representation.
    QCOMPARE(result.partialFragments().size(), 1);
    QCOMPARE(result.partialPresentation(), QStringLiteral("cover"));
    const QVariantList covers = result.translatedLines();
    QCOMPARE(covers.size(), 1);
    QCOMPARE(covers.first().toMap().value(QStringLiteral("screenOnly")).toBool(), true);
    QCOMPARE(covers.first().toMap().value(QStringLiteral("sourceStart")).toInt(), start);
    QCOMPARE(covers.first().toMap().value(QStringLiteral("sourceLength")).toInt(), 3);
    QVERIFY(result.partialCallout().isEmpty());
    // Save image and Copy image keep the page: the word cover is screen only.
    QCOMPARE(result.compositedImage(true), image);

    // The request carries the exact fragment plus the source line as context.
    QCOMPARE(server.bodies.size(), 1);
    const QByteArray &request = server.bodies.first();
    const int bodyStart = int(request.indexOf("\r\n\r\n")) + 4;
    const QJsonObject payload = QJsonDocument::fromJson(request.mid(bodyStart)).object();
    const QJsonArray messages = payload.value(QStringLiteral("messages")).toArray();
    QVERIFY(messages.size() >= 2);
    const QString system = messages.at(0).toObject().value(QStringLiteral("content")).toString();
    const QString user = messages.last().toObject().value(QStringLiteral("content")).toString();
    QVERIFY2(system.contains(QStringLiteral("Polish")), qPrintable(system));
    QVERIFY2(system.contains(QStringLiteral("only the text under")), qPrintable(system));
    QVERIFY2(user.contains(QStringLiteral("Context: One two three")), qPrintable(user));
    QVERIFY2(user.contains(QStringLiteral("Selected text: two")), qPrintable(user));
    QVERIFY2(!user.contains(QStringLiteral("Selected text: One")), qPrintable(user));

    // A range that crosses a line boundary but is not whole lines sends exactly
    // that range, with both touched lines as context, and gets one callout
    // anchored to the first fragment: covers are single-line only.
    const int crossStart = result.text().indexOf(QStringLiteral("three"));
    const int crossLength = result.text().indexOf(QStringLiteral("Second")) + 6 - crossStart;
    QVERIFY(crossLength > 0);
    result.translateRange(crossStart, crossLength);
    QVERIFY(result.translationPartialText());
    QCOMPARE(result.partialFragments().size(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!result.translating(), 5000);
    QCOMPARE(result.partialPresentation(), QStringLiteral("callout"));
    QVERIFY(result.translatedLines().isEmpty());
    QCOMPARE(result.partialCallout().value(QStringLiteral("source")).toString(),
             QStringLiteral("three\nSecond"));
    QCOMPARE(result.partialCallout().value(QStringLiteral("translation")).toString(),
             QStringLiteral("Hello world"));
    const QJsonObject crossPayload =
        QJsonDocument::fromJson(server.bodies.last().mid(server.bodies.last().indexOf("\r\n\r\n") + 4))
            .object();
    const QString crossUser = crossPayload.value(QStringLiteral("messages")).toArray().last()
                                  .toObject().value(QStringLiteral("content")).toString();
    QVERIFY2(crossUser.contains(QStringLiteral("Context: One two three\nSecond line")),
             qPrintable(crossUser));
    QVERIFY2(crossUser.contains(QStringLiteral("Selected text: three\nSecond")),
             qPrintable(crossUser));
}

// A line over the per-request budget is split at sentence boundaries and sent
// as separate numbered items, but it must come back as ONE cover with the parts
// joined: the picture has one box for it, not two.
// M4.6: a readable partial answer covers exactly the glyphs the reader
// selected, not the whole line, and the cover is screen only.
void OcrResultTest::partialAnswerCoversOnlyItsGlyphs()
{
    QImage image(400, 120, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    const QString line = QStringLiteral("One two three");
    const QRect box(20, 16, 340, 28);
    OcrResultController result;
    result.setResult(image, glyphWordsFor({line}, {box}), QString());
    const int start = result.text().indexOf(QStringLiteral("two"));
    QVERIFY(start >= 0);
    result.setPartialTranslationForPreview(start, 3, QStringLiteral("dwa"));

    QCOMPARE(result.partialPresentation(), QStringLiteral("cover"));
    QCOMPARE(result.partialFragments().size(), 1);
    const QVariantMap fragment = result.partialFragments().first().toMap();
    QCOMPARE(fragment.value(QStringLiteral("start")).toInt(), start);
    QCOMPARE(fragment.value(QStringLiteral("length")).toInt(), 3);
    // The word's own glyphs, not its line: a cover this narrow is the whole
    // point of the change.
    QVERIFY(entryRect(fragment).width() < box.width() / 2);
    QVERIFY(entryRect(fragment).height() >= box.height());

    const QVariantList covers = result.translatedLines();
    QCOMPARE(covers.size(), 1);
    const QVariantMap cover = covers.first().toMap();
    QCOMPARE(cover.value(QStringLiteral("screenOnly")).toBool(), true);
    QCOMPARE(cover.value(QStringLiteral("text")).toString(), QStringLiteral("dwa"));
    QCOMPARE(cover.value(QStringLiteral("sourceStart")).toInt(), start);
    QCOMPARE(cover.value(QStringLiteral("sourceLength")).toInt(), 3);
    QVERIFY(cover.value(QStringLiteral("fontSize")).toInt()
            >= OcrResultController::kMinPartialCoverPixelSize);
    QVERIFY(!cover.value(QStringLiteral("overflow")).toBool());
    QVERIFY(result.partialCallout().isEmpty());
    QCOMPARE(result.compositedImage(true), image);
    QCOMPARE(OcrResultController::composeOverlay(image, covers), image);
}

// M4.6: an answer too long to read on the selected glyphs goes to one callout
// anchored to them, and leaves translatedLines empty so nothing is painted.
void OcrResultTest::longPartialAnswerShowsACallout()
{
    QImage image(400, 120, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    const QString line = QStringLiteral("One two three");
    const QRect box(20, 16, 340, 28);
    OcrResultController result;
    result.setResult(image, glyphWordsFor({line}, {box}), QString());
    const int start = result.text().indexOf(QStringLiteral("two"));
    QVERIFY(start >= 0);
    const QString answer =
        QStringLiteral("A translation far too long to read on one three-letter word");
    result.setPartialTranslationForPreview(start, 3, answer);

    QCOMPARE(result.translation(), answer);
    QVERIFY(result.translationPartialText());
    QCOMPARE(result.partialPresentation(), QStringLiteral("callout"));
    QVERIFY(result.translatedLines().isEmpty());
    QCOMPARE(result.compositedImage(true), image);
    const QVariantMap callout = result.partialCallout();
    QCOMPARE(callout.value(QStringLiteral("source")).toString(), QStringLiteral("two"));
    QCOMPARE(callout.value(QStringLiteral("translation")).toString(), answer);
    const QRect anchor = entryRect(callout);
    QVERIFY(anchor.width() > 0 && anchor.height() > 0);
    QVERIFY(anchor.width() < box.width() / 2);
    QVERIFY(anchor.intersects(box));

    // A second answer on the same controller has to re-decide. A stale "cover"
    // from the first answer must not keep a long one off the callout (the F8
    // check reuses one controller exactly like this).
    result.setPartialTranslationForPreview(start, 3, QStringLiteral("dwa"));
    QCOMPARE(result.partialPresentation(), QStringLiteral("cover"));
    result.setPartialTranslationForPreview(start, 3, answer);
    QCOMPARE(result.partialPresentation(), QStringLiteral("callout"));
}

// M4.6: covers are single-line only. A selection crossing a line boundary gets
// one callout for the whole answer, anchored at the first fragment.
void OcrResultTest::partialAcrossLinesShowsOneCallout()
{
    QImage image(400, 140, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    const QVector<QRect> boxes{QRect(20, 16, 300, 28), QRect(20, 78, 300, 28)};
    OcrResultController result;
    result.setResult(image,
                     glyphWordsFor({QStringLiteral("One two"), QStringLiteral("three four")},
                                   boxes),
                     QString());
    const int start = result.text().indexOf(QStringLiteral("two"));
    const int end = result.text().indexOf(QStringLiteral("three")) + 5;
    QVERIFY(start >= 0 && end > start);
    result.setPartialTranslationForPreview(start, end - start, QStringLiteral("dwa trzy"));

    QCOMPARE(result.partialFragments().size(), 2);
    QCOMPARE(result.partialPresentation(), QStringLiteral("callout"));
    QVERIFY(result.translatedLines().isEmpty());
    const QVariantMap callout = result.partialCallout();
    QCOMPARE(callout.value(QStringLiteral("source")).toString(), QStringLiteral("two\nthree"));
    QCOMPARE(callout.value(QStringLiteral("translation")).toString(), QStringLiteral("dwa trzy"));
    // Anchored to the first fragment's line, not the second one's.
    QVERIFY(entryRect(callout).intersects(boxes.first()));
    QVERIFY(!entryRect(callout).intersects(boxes.last()));
}

// M4.6: streaming shows the answer in the callout as it arrives; the
// cover/callout decision runs once, at Done.
void OcrResultTest::partialCalloutStreamsWithTheAnswer()
{
    MockTranslationEndpoint server(MockTranslationEndpoint::SlowSse);
    QVERIFY(server.start());
    Settings settings;
    settings.setTranslationEndpoint(server.base());
    settings.setTranslationModel(QStringLiteral("mock"));
    settings.setTranslationTarget(QStringLiteral("Polish"));

    OcrResultController result(&settings);
    QImage image(400, 120, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    const QRect box(20, 16, 340, 28);
    result.setResult(image, glyphWordsFor({QStringLiteral("One two three")}, {box}), QString());
    const int start = result.text().indexOf(QStringLiteral("two"));
    QVERIFY(start >= 0);
    result.translateRange(start, 3);
    QVERIFY(result.translating());
    QCOMPARE(result.partialPresentation(), QStringLiteral("callout"));
    QCOMPARE(result.partialCallout().value(QStringLiteral("source")).toString(),
             QStringLiteral("two"));

    QTRY_VERIFY_WITH_TIMEOUT(result.translating()
                                 && result.translation() == QStringLiteral("Hello"),
                             3000);
    QCOMPARE(result.partialCallout().value(QStringLiteral("translation")).toString(),
             QStringLiteral("Hello"));

    QTRY_VERIFY_WITH_TIMEOUT(!result.translating(), 5000);
    QCOMPARE(result.translation(), QStringLiteral("Hello world"));
    QCOMPARE(result.partialPresentation(), QStringLiteral("cover"));
    QVERIFY(result.partialCallout().isEmpty());
    QVERIFY(result.translatedLines().first().toMap()
                .value(QStringLiteral("screenOnly")).toBool());
}

// M4.6: composeOverlay() is the Save/Copy image path, so it must skip a
// screen-only entry instead of baking a word cover into the page.
void OcrResultTest::screenOnlyCoverStaysOutOfTheComposite()
{
    QImage image(200, 80, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    QVariantMap painted{{QStringLiteral("x"), 10},      {QStringLiteral("y"), 10},
                        {QStringLiteral("width"), 40},  {QStringLiteral("height"), 20},
                        {QStringLiteral("background"), QStringLiteral("#ff0000")},
                        {QStringLiteral("foreground"), QStringLiteral("#000000")},
                        {QStringLiteral("text"), QString()},
                        {QStringLiteral("fontSize"), 10},
                        {QStringLiteral("rtl"), false},
                        {QStringLiteral("overflow"), false}};
    QVariantMap screenOnly = painted;
    screenOnly.insert(QStringLiteral("x"), 120);
    screenOnly.insert(QStringLiteral("background"), QStringLiteral("#0000ff"));
    screenOnly.insert(QStringLiteral("screenOnly"), true);

    const QImage composite =
        OcrResultController::composeOverlay(image, {painted, screenOnly});
    QCOMPARE(composite.pixel(30, 20), qRgb(255, 0, 0));
    QCOMPARE(composite.pixel(140, 20), qRgb(250, 250, 245));
    // Nothing but screen-only covers means the page is handed over untouched.
    QCOMPARE(OcrResultController::composeOverlay(image, {screenOnly}), image);
}

void OcrResultTest::longLineIsSplitAndRejoined()
{
    MockTranslationEndpoint server(MockTranslationEndpoint::NumberedSse);
    QVERIFY(server.start());
    Settings settings;
    settings.setTranslationEndpoint(server.base());
    settings.setTranslationModel(QStringLiteral("mock"));
    settings.setTranslationTarget(QStringLiteral("Polish"));

    OcrResultController result(&settings);
    QString longLine;
    for (int i = 0; i < 160; ++i)
        longLine += QStringLiteral("Sentence number %1 is here. ").arg(i);
    OcrWord word;
    word.line = 0;
    word.text = longLine.trimmed();
    word.rect = QRect(10, 10, 180, 24);
    QImage image(200, 120, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    QPainter painter(&image);
    painter.fillRect(QRect(12, 18, 170, 6), QColor(20, 20, 20));
    painter.end();
    result.setResult(image, {word}, QString());
    QCOMPARE(result.lines().size(), 1);

    result.translate();
    QTRY_VERIFY_WITH_TIMEOUT(!result.translating(), 5000);
    QCOMPARE(result.translationError(), QString());
    QCOMPARE(result.translationLineCount(), 1);
    QVERIFY2(result.translationSplitLines() >= 1,
             "the 160-sentence line has to be split for the request budget");
    QCOMPARE(result.translatedLines().size(), 1);
    const QString cover =
        result.translatedLines().at(0).toMap().value(QStringLiteral("text")).toString();
    QVERIFY2(cover.contains(QStringLiteral("Translated part 1")), qPrintable(cover));
    QVERIFY2(cover.contains(QStringLiteral("Translated part 2")), qPrintable(cover));
    // The parts are one line, not two: the pane's joined translation is the
    // same string the cover carries.
    QCOMPARE(result.translation(), cover);
}

// A transport error or 5xx gets one retry after a short backoff, and the user
// never sees the first failure.
void OcrResultTest::retriesAChunkOnceThenSucceeds()
{
    MockTranslationEndpoint server(MockTranslationEndpoint::FailOnceNumberedSse);
    QVERIFY(server.start());
    Settings settings;
    settings.setTranslationEndpoint(server.base());
    settings.setTranslationModel(QStringLiteral("mock"));

    OcrResultController result(&settings);
    QVector<OcrWord> words;
    for (int i = 0; i < 2; ++i) {
        OcrWord word;
        word.line = i;
        word.text = QStringLiteral("line %1").arg(i);
        word.rect = QRect(10, 10 + i * 30, 120, 20);
        words.append(word);
    }
    QImage image(180, 100, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    result.setResult(image, words, QString());

    result.translate();
    QCOMPARE(result.translationChunkCount(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!result.translating(), 5000);
    QCOMPARE(result.translationError(), QString());
    QCOMPARE(result.translationChunksDone(), 1);
    QCOMPARE(result.translationChunkIndex(), 0);
    QCOMPARE(result.translation(), QStringLiteral("Translated part 1\nTranslated part 2"));
    QCOMPARE(result.translatedLines().size(), 2);
    QCOMPARE(server.bodies.size(), 2);
}

// One chunk failing for good must not throw away the chunks that worked: the
// finished lines stay, and the window names the part that did not translate.
void OcrResultTest::partialTranslationNamesFailedLines()
{
    MockTranslationEndpoint server(MockTranslationEndpoint::FailSecondChunk);
    QVERIFY(server.start());
    Settings settings;
    settings.setTranslationEndpoint(server.base());
    settings.setTranslationModel(QStringLiteral("mock"));

    OcrResultController result(&settings);
    QVector<OcrWord> words;
    for (int i = 0; i < 45; ++i) {
        OcrWord word;
        word.line = i;
        word.text = QStringLiteral("line %1").arg(i);
        word.rect = QRect(10, 10 + i * 30, 150, 20);
        words.append(word);
    }
    QImage image(180, 1400, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    result.setResult(image, words, QString());
    QCOMPARE(result.lines().size(), 45);

    result.translate();
    QCOMPARE(result.translationChunkCount(), 2);
    QCOMPARE(result.translationChunkIndex(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!result.translating(), 8000);
    QCOMPARE(result.translationChunkIndex(), 0);
    QCOMPARE(result.translationChunksDone(), 2);
    // The first 40 lines are translated and shown; lines 41-45 are named.
    QCOMPARE(result.translatedLines().size(), 40);
    QVERIFY(!result.translation().isEmpty());
    QVERIFY2(result.translationError().contains(QStringLiteral("41-45")),
             qPrintable(result.translationError()));
    QVERIFY(result.translationError().contains(QStringLiteral("server is down")));
    // Chunk one plus two attempts at the failing chunk, each a real request.
    QCOMPARE(server.bodies.size(), 3);

    // The same failure for a selection names the SOURCE lines the reader
    // counts, not the position inside the selection.
    MockTranslationEndpoint selectedServer(MockTranslationEndpoint::FailSecondChunk);
    QVERIFY(selectedServer.start());
    Settings selectedSettings;
    selectedSettings.setTranslationEndpoint(selectedServer.base());
    selectedSettings.setTranslationModel(QStringLiteral("mock"));
    OcrResultController selected(&selectedSettings);
    selected.setResult(image, words, QString());
    selected.translateRange(0, selected.text().size());
    QVERIFY(selected.translationSelectionOnly());
    QTRY_VERIFY_WITH_TIMEOUT(!selected.translating(), 8000);
    QCOMPARE(selected.translatedLines().size(), 40);
    QVERIFY2(selected.translationError().contains(QStringLiteral("Selected lines 41-45")),
             qPrintable(selected.translationError()));
}

void OcrResultTest::translatedLinesIndexThePane()
{
    QImage image(160, 80, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    QPainter painter(&image);
    painter.fillRect(QRect(10, 10, 60, 14), QColor(20, 20, 20));
    painter.fillRect(QRect(10, 40, 90, 14), QColor(20, 20, 20));
    painter.end();

    const QVariantList lines = {
        QVariantMap{{QStringLiteral("text"), QStringLiteral("Hi")},
                    {QStringLiteral("x"), 10},
                    {QStringLiteral("y"), 10},
                    {QStringLiteral("width"), 60},
                    {QStringLiteral("height"), 14}},
        QVariantMap{{QStringLiteral("text"), QStringLiteral("Second line")},
                    {QStringLiteral("x"), 10},
                    {QStringLiteral("y"), 40},
                    {QStringLiteral("width"), 90},
                    {QStringLiteral("height"), 14}},
    };
    const QStringList translated{QStringLiteral("Cześć"), QStringLiteral("Druga linia")};
    QString joined;
    const QVariantList overlay =
        OcrResultController::translatedLineMap(lines, translated, image, &joined);

    QCOMPARE(overlay.size(), 2);
    QCOMPARE(joined, QStringLiteral("Cześć\nDruga linia"));
    const QVariantMap first = overlay.at(0).toMap();
    const QVariantMap second = overlay.at(1).toMap();
    QCOMPARE(first.value(QStringLiteral("start")).toInt(), 0);
    QCOMPARE(first.value(QStringLiteral("length")).toInt(), 5);
    QCOMPARE(joined.mid(0, 5), QStringLiteral("Cześć"));
    QCOMPARE(second.value(QStringLiteral("start")).toInt(), 6);
    QCOMPARE(second.value(QStringLiteral("length")).toInt(), 11);
    QCOMPARE(joined.mid(6, 11), QStringLiteral("Druga linia"));
    // The cover is the box grown by the glyph overshoot: 30 percent of the line
    // height sideways (4 px here) and 1 px up/down, so the last letter is hidden
    // without the line above being touched.
    QCOMPARE(second.value(QStringLiteral("x")).toInt(), 6);
    QCOMPARE(second.value(QStringLiteral("y")).toInt(), 39);
    QCOMPARE(second.value(QStringLiteral("width")).toInt(), 98);
    QCOMPARE(second.value(QStringLiteral("height")).toInt(), 16);
    // A missing translation still produces a box, so nothing shifts.
    const QVariantList shortMap =
        OcrResultController::translatedLineMap(lines, {QStringLiteral("Only")}, image, &joined);
    QCOMPARE(shortMap.size(), 2);
    QCOMPARE(joined, QStringLiteral("Only\n"));
}

void OcrResultTest::lineColorsFollowThePicture()
{
    // Dark page, light glyphs: the cover must be dark and the text light.
    QImage dark(120, 60, QImage::Format_RGB32);
    dark.fill(QColor(24, 24, 30));
    QPainter painter(&dark);
    // A glyph stroke inside the line box, not a filled box: a real line box is
    // mostly page with strokes in it, and the mode has to find the page.
    painter.fillRect(QRect(12, 24, 66, 8), QColor(240, 240, 235));
    painter.end();
    const OcrResultController::LineColors onDark =
        OcrResultController::sampleLineColors(dark, QRect(10, 20, 70, 16));
    QVERIFY(onDark.background.lightnessF() < 0.2f);
    QVERIFY(onDark.foreground.lightnessF() > 0.8f);

    // Light page, pale ink: the sampled pair would be unreadable, so the
    // foreground falls back to black instead of copying the page.
    QImage pale(120, 60, QImage::Format_RGB32);
    pale.fill(QColor(250, 250, 245));
    QPainter second(&pale);
    second.fillRect(QRect(12, 24, 66, 8), QColor(225, 225, 220));
    second.end();
    const OcrResultController::LineColors onPale =
        OcrResultController::sampleLineColors(pale, QRect(10, 20, 70, 16));
    QVERIFY(onPale.background.lightnessF() > 0.8f);
    QVERIFY(onPale.foreground.lightnessF() < 0.1f);

    // No image and an empty box are legal during a QR-only result.
    const OcrResultController::LineColors none =
        OcrResultController::sampleLineColors(QImage(), QRect());
    QCOMPARE(none.background, QColor(255, 255, 255));
    QCOMPARE(none.foreground, QColor(0, 0, 0));
}

void OcrResultTest::longPagesSplitIntoRequests()
{
    QStringList lines;
    for (int i = 0; i < 95; ++i)
        lines.append(QStringLiteral("line %1").arg(i));
    const QVector<QStringList> chunks = OcrResultController::chunkLines(lines);
    QCOMPARE(chunks.size(), 3);
    QCOMPARE(chunks.at(0).size(), 40);
    QCOMPARE(chunks.at(2).size(), 15);
    QCOMPARE(chunks.at(0).at(0), QStringLiteral("line 0"));
    QCOMPARE(chunks.at(2).at(14), QStringLiteral("line 94"));
    // Order survives the split: the map puts chunk N back after chunk N-1.
    QStringList rejoined;
    for (const QStringList &chunk : chunks)
        rejoined += chunk;
    QCOMPARE(rejoined, lines);
    QVERIFY(OcrResultController::chunkLines({}).isEmpty());
    QCOMPARE(OcrResultController::chunkLines({QStringLiteral("a")}, 0).size(), 1);
}

// A service that echoes the input is not a translation the window should
// present silently; the comparison ignores the re-wrapping such answers often
// carry.
void OcrResultTest::spotsAnUnchangedAnswer()
{
    QVERIFY(OcrResultController::translationLooksUnchanged(
        QStringLiteral("从磁盘打开任何文件\n搜索截图"),
        QStringLiteral("从磁盘打开任何文件\n搜索截图")));
    QVERIFY(OcrResultController::translationLooksUnchanged(
        QStringLiteral("Hello world"), QStringLiteral("  Hello   world  ")));
    QVERIFY(!OcrResultController::translationLooksUnchanged(
        QStringLiteral("你好"), QStringLiteral("Hello")));
    QVERIFY(!OcrResultController::translationLooksUnchanged(QString(), QString()));
}

// The cover's font size is the ONE decision the screen and the saved picture
// share. It has to fit the box, sit between the 5 px floor and the size
// Text.Fit used to start from, and be the largest size that still fits.
void OcrResultTest::overlayFontSizeFitsTheBox()
{
    const FourLineFixture fixture = fourLineFixture(400, 240, 20, 30, 52);
    QCOMPARE(fixture.overlay.size(), 4);
    for (int i = 0; i < fixture.overlay.size(); ++i) {
        const QVariantMap entry = fixture.overlay.at(i).toMap();
        const QRect cover = entryRect(entry);
        const QString text = entry.value(QStringLiteral("text")).toString();
        const int size = entry.value(QStringLiteral("fontSize")).toInt();
        const bool rtl = entry.value(QStringLiteral("rtl")).toBool();
        const int maxSize = qMax(8, cover.height());
        QVERIFY2(size >= OcrResultController::kMinFitPixelSize && size <= maxSize,
                 qPrintable(QStringLiteral("line %1 size %2 outside [5, %3]")
                                .arg(i).arg(size).arg(maxSize)));
        QVERIFY2(textFits(text, cover.size(), size, rtl),
                 qPrintable(QStringLiteral("line %1 does not fit at its own size %2")
                                .arg(i).arg(size)));
        // Largest that fits: one pixel more must already overflow the box, or
        // the fit stopped early and the text is smaller than it had to be.
        if (size < maxSize) {
            QVERIFY2(!textFits(text, cover.size(), size + 1, rtl),
                     qPrintable(QStringLiteral("line %1 could be larger than %2")
                                    .arg(i).arg(size)));
        }
        QVERIFY(!entry.value(QStringLiteral("overflow")).toBool());
    }

    // A translation too long for a tiny box cannot fit at the floor: it is
    // clipped, and the map says so instead of pretending it fits. The window
    // and the composite draw the overflow marker off this flag.
    QImage tiny(120, 60, QImage::Format_RGB32);
    tiny.fill(QColor(250, 250, 245));
    QString joined;
    const QVariantList map = OcrResultController::translatedLineMap(
        {lineEntry(QStringLiteral("Hello"), QRect(10, 10, 24, 12))},
        {QStringLiteral("A translation far too long for a 24 px box")}, tiny, &joined);
    QCOMPARE(map.size(), 1);
    const QVariantMap narrow = map.at(0).toMap();
    QCOMPARE(narrow.value(QStringLiteral("fontSize")).toInt(),
             OcrResultController::kMinFitPixelSize);
    QCOMPARE(narrow.value(QStringLiteral("overflow")).toBool(), true);

    // An empty box (a QR payload's line map, a zero-height cover) still answers
    // with the floor rather than nothing, so no caller divides by zero.
    bool fits = true;
    QCOMPARE(OcrResultController::fittedPixelSize(QStringLiteral("x"), QSize(0, 0), 20, &fits),
             OcrResultController::kMinFitPixelSize);
    QCOMPARE(fits, false);
    QCOMPARE(OcrResultController::fittedPixelSize(QString(), QSize(10, 10), 20, &fits), 20);
    QCOMPARE(fits, true);
}

// A CJK glyph fills nearly the whole em box, so a cover at the largest size
// that fits reads larger than the page it replaces. Such a cover is set 10%
// below that size; a Latin cover keeps the exact fit. The composite reads the
// same fontSize, so the saved picture and the screen stay in step.
void OcrResultTest::cjkCoverSetsItselfSlightlySmaller()
{
    QImage image(340, 90, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    const QRect box(10, 10, 300, 30);
    QString joined;
    const QVariantMap latin = OcrResultController::translatedLineMap(
        {lineEntry(QStringLiteral("Hello"), box)}, {QStringLiteral("Hi")}, image, &joined)
                                  .first().toMap();
    const QVariantMap cjk = OcrResultController::translatedLineMap(
        {lineEntry(QStringLiteral("你好"), box)}, {QStringLiteral("你好世界")}, image, &joined)
                                .first().toMap();
    QVERIFY(!latin.value(QStringLiteral("overflow")).toBool());
    QVERIFY(!cjk.value(QStringLiteral("overflow")).toBool());
    const int latinSize = latin.value(QStringLiteral("fontSize")).toInt();
    const int cjkSize = cjk.value(QStringLiteral("fontSize")).toInt();
    QVERIFY2(cjkSize < latinSize, qPrintable(QStringLiteral("CJK %1 is not below Latin %2")
                                                 .arg(cjkSize).arg(latinSize)));
    QCOMPARE(cjkSize, qMax(OcrResultController::kMinFitPixelSize,
                           int(std::lround(latinSize * 0.9))));
    QVERIFY(textFits(QStringLiteral("你好世界"), entryRect(cjk).size(), cjkSize, false));
    QVERIFY(latinSize <= qMax(8, entryRect(latin).height()));
}

// The window scales a capture down when it does not fit, but the picture handed
// to Save, Copy image and the editor must stay at the capture's own resolution.
void OcrResultTest::compositeKeepsSourceResolution()
{
    const FourLineFixture fixture = fourLineFixture(3840, 2160, 120, 120, 220);
    const QImage composite = OcrResultController::composeOverlay(fixture.image, fixture.overlay);
    QCOMPARE(composite.size(), fixture.image.size());
    QCOMPARE(composite.size(), QSize(3840, 2160));

    // The Original layer is the untouched capture; with no covers (a QR
    // payload) the Translation layer is the same picture.
    OcrResultController result;
    result.setResult(fixture.image, {}, QStringLiteral("https://example.test/payload"));
    QCOMPARE(result.compositedImage(false), fixture.image);
    QCOMPARE(result.compositedImage(true), fixture.image);
    QCOMPARE(result.compositedImage(true).size(), fixture.image.size());
}

// The saved picture must draw exactly what the window shows: the translation
// inside its cover, and nothing at all outside the covers.
void OcrResultTest::compositeDrawsOnlyInsideCovers()
{
    const FourLineFixture fixture = fourLineFixture(400, 240, 20, 30, 52);
    const QImage source = fixture.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage composite = OcrResultController::composeOverlay(fixture.image, fixture.overlay);
    QCOMPARE(composite.size(), source.size());

    // Everything that is not one of the padded covers is the untouched capture:
    // no ink and no cover spills onto the line above or beside it (2 px of
    // tolerance for the rounded-corner antialiasing).
    for (int y = 0; y < composite.height(); ++y) {
        for (int x = 0; x < composite.width(); ++x) {
            bool nearCover = false;
            for (const QRect &cover : fixture.covers) {
                if (cover.adjusted(-2, -2, 2, 2).contains(x, y)) {
                    nearCover = true;
                    break;
                }
            }
            if (nearCover)
                continue;
            QVERIFY2(composite.pixel(x, y) == source.pixel(x, y),
                     qPrintable(QStringLiteral("pixel %1,%2 changed outside every cover")
                                    .arg(x).arg(y)));
        }
    }

    // Each cover really carries its translation in the sampled ink colour.
    for (int i = 0; i < fixture.overlay.size(); ++i) {
        const QVariantMap entry = fixture.overlay.at(i).toMap();
        const QRect cover = entryRect(entry);
        const QColor ink(entry.value(QStringLiteral("foreground")).toString());
        int inkPixels = 0;
        for (int y = cover.top(); y <= cover.bottom(); ++y) {
            for (int x = cover.left(); x <= cover.right(); ++x) {
                const QRgb pixel = composite.pixel(x, y);
                if (qRed(pixel) == ink.red() && qGreen(pixel) == ink.green()
                    && qBlue(pixel) == ink.blue())
                    ++inkPixels;
            }
        }
        QVERIFY2(inkPixels > 0, qPrintable(QStringLiteral("cover %1 has no text").arg(i)));
    }
}

// A click on the picture has to find the same line at every zoom level. The
// window converts its scaled, panned pane point into image pixels; from there
// the hit test is scale-free, which is what this pins.
void OcrResultTest::clickFindsTheLineAtEveryScale()
{
    const FourLineFixture fixture = fourLineFixture(400, 240, 20, 30, 52);

    OcrResultController result;
    QVector<OcrWord> words;
    for (int i = 0; i < fixture.lines.size(); ++i) {
        const QVariantMap line = fixture.lines.at(i).toMap();
        OcrWord word;
        word.line = i;
        word.text = line.value(QStringLiteral("text")).toString();
        word.rect = entryRect(line);
        words.append(word);
    }
    result.setResult(fixture.image, words, QString());
    for (int i = 0; i < fixture.lines.size(); ++i) {
        const QRect box = entryRect(fixture.lines.at(i).toMap());
        QCOMPARE(result.lineAt(QPointF(box.center()) + QPointF(0.5, 0.5), false), i);
    }
    QCOMPARE(result.lineAt(QPointF(1, 1), false), -1);
    QCOMPARE(result.lineAt(QPointF(-20, -20), false), -1);

    for (int i = 0; i < fixture.covers.size(); ++i) {
        const QRect cover = fixture.covers.at(i);
        for (const qreal scale : {0.5, 1.0, 1.25, 2.0, 4.0}) {
            const QPointF pan(13.0 * scale, -7.0);
            const QPointF panePoint = QPointF(cover.center()) * scale + pan;
            const QPointF imagePoint = (panePoint - pan) / scale;
            QCOMPARE(OcrResultController::lineIndexAt(fixture.overlay, imagePoint), i);
        }
    }
    QCOMPARE(OcrResultController::lineIndexAt(fixture.overlay, QPointF(-5, -5)), -1);
    QCOMPARE(OcrResultController::lineIndexAt({}, QPointF(10, 10)), -1);
}

// Two columns on one visual row: the sideways overshoot stops in the gutter, so
// a cover can never hide the first letter of the column beside it.
void OcrResultTest::coverStopsAtTheNeighbourColumn()
{
    QImage page(300, 80, QImage::Format_RGB32);
    page.fill(QColor(250, 250, 245));
    QPainter painter(&page);
    painter.fillRect(QRect(12, 16, 56, 4), QColor(20, 20, 20));
    painter.fillRect(QRect(76, 16, 56, 4), QColor(20, 20, 20));
    painter.end();

    // A 14 px line wants a 4 px overshoot on each side; the 4 px gutter between
    // the boxes allows 2.
    const QVariantList columns{
        lineEntry(QStringLiteral("Left"), QRect(10, 10, 60, 14)),
        lineEntry(QStringLiteral("Right"), QRect(74, 10, 60, 14)),
    };
    QString joined;
    const QVariantList map = OcrResultController::translatedLineMap(
        columns, {QStringLiteral("Lewa"), QStringLiteral("Prawa")}, page, &joined);
    QCOMPARE(map.size(), 2);
    const QRect left = entryRect(map.at(0).toMap());
    const QRect right = entryRect(map.at(1).toMap());
    // Left cover: x 8..69 would be the full pad; capped it must stop at 71 and
    // never reach the right box (left edge 74).
    QVERIFY2(left.right() < 74, qPrintable(QStringLiteral("left cover ends at %1")
                                              .arg(left.right())));
    QVERIFY2(right.left() > 69, qPrintable(QStringLiteral("right cover starts at %1")
                                              .arg(right.left())));
    // The covers may touch the boxes but not each other.
    QVERIFY(left.right() < right.left());
    // The vertical overshoot is untouched: it is never what hides a neighbour.
    QCOMPARE(left.top(), 9);
    QCOMPARE(left.bottom(), 24);
}

// An RTL target has to render right to left in the cover: the map carries the
// direction so the window right-aligns the line and the composite does the same.
void OcrResultTest::rtlTranslationAlignsRight()
{
    QVERIFY(OcrResultController::lineIsRtl(QStringLiteral("\u0645\u0631\u062d\u0628\u0627")));
    QVERIFY(OcrResultController::lineIsRtl(QStringLiteral("\u05e9\u05dc\u05d5\u05dd")));
    QVERIFY(!OcrResultController::lineIsRtl(QStringLiteral("Hello")));
    QVERIFY(!OcrResultController::lineIsRtl(QStringLiteral("\u4f60\u597d")));
    QVERIFY(!OcrResultController::lineIsRtl(QStringLiteral("123 456")));
    QVERIFY(!OcrResultController::lineIsRtl(QString()));

    QImage image(200, 80, QImage::Format_RGB32);
    image.fill(QColor(250, 250, 245));
    const QVariantList lines = {
        lineEntry(QStringLiteral("Hello"), QRect(10, 10, 80, 20)),
        lineEntry(QStringLiteral("Hello"), QRect(10, 40, 80, 20)),
    };
    QString joined;
    const QVariantList map = OcrResultController::translatedLineMap(
        lines, {QStringLiteral("\u0645\u0631\u062d\u0628\u0627"), QStringLiteral("Hello")},
        image, &joined);
    QCOMPARE(map.size(), 2);
    QCOMPARE(map.at(0).toMap().value(QStringLiteral("rtl")).toBool(), true);
    QCOMPARE(map.at(1).toMap().value(QStringLiteral("rtl")).toBool(), false);
}

QTEST_MAIN(OcrResultTest)
#include "OcrResultTest.moc"
