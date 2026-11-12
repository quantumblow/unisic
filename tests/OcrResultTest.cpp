#include <QtTest>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>

#include "ocr/OcrResultController.h"

// The result window's line ranges are the only thing that makes a click on the
// picture select the right text. Pin the text/offset/rect contract without any
// OCR model or display server.
class OcrResultTest : public QObject
{
    Q_OBJECT

private slots:
    void rangesMatchTheText();
    void payloadWithoutGeometry();
    void translatedLinesIndexThePane();
    void lineColorsFollowThePicture();
    void longPagesSplitIntoRequests();
};

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

QTEST_MAIN(OcrResultTest)
#include "OcrResultTest.moc"
