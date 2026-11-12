#include <QtTest>
#include <QImage>
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

QTEST_MAIN(OcrResultTest)
#include "OcrResultTest.moc"
