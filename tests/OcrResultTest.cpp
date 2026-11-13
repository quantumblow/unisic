#include <QtTest>
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
    void payloadWithoutGeometry();
    void selectionMapsToWholeLines();
    void streamingFillsThePaneAndCancelStopsIt();
    void longLineIsSplitAndRejoined();
    void retriesAChunkOnceThenSucceeds();
    void partialTranslationNamesFailedLines();
    void translatedLinesIndexThePane();
    void lineColorsFollowThePicture();
    void longPagesSplitIntoRequests();
    void spotsAnUnchangedAnswer();
    void overlayFontSizeFitsTheBox();
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

// A line over the per-request budget is split at sentence boundaries and sent
// as separate numbered items, but it must come back as ONE cover with the parts
// joined: the picture has one box for it, not two.
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
    QTRY_VERIFY_WITH_TIMEOUT(!result.translating(), 5000);
    QCOMPARE(result.translationError(), QString());
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
    QTRY_VERIFY_WITH_TIMEOUT(!result.translating(), 8000);
    // The first 40 lines are translated and shown; lines 41-45 are named.
    QCOMPARE(result.translatedLines().size(), 40);
    QVERIFY(!result.translation().isEmpty());
    QVERIFY2(result.translationError().contains(QStringLiteral("41-45")),
             qPrintable(result.translationError()));
    QVERIFY(result.translationError().contains(QStringLiteral("server is down")));
    // Chunk one plus two attempts at the failing chunk, each a real request.
    QCOMPARE(server.bodies.size(), 3);
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
