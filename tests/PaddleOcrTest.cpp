#include <QtTest>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include "ocr/PaddleOcr.h"

// The bundled PP-OCRv6 tiny tier and the routing that follows it. Neither an
// ONNX Runtime nor a downloaded model is needed: the tiny files are part of the
// test binary (CMakeLists.txt embeds the same qrc in it), and the language map
// is a pure function of the tier.
//
// The point of these tests is "works out of the box": with an empty data dir,
// the effective tier is the included tiny, nothing reports a download, and
// Japanese is routed to Tesseract until Small or Medium is installed.
class PaddleOcrTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void effectiveTierFallsBackToBundledTiny();
    void bundledFilesResolveFromTheBinary();
    void tierStatesListAllThree();
    void tinyRoutesJapaneseToTesseract();
    void smallAndMediumReadJapanese();

private:
    static void clearDownloads();
};

void PaddleOcrTest::clearDownloads()
{
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
         + QStringLiteral("/paddleocr")).removeRecursively();
}

void PaddleOcrTest::initTestCase()
{
    // A developer machine or a previous run may have Small downloaded; this
    // class is about a fresh install, so the models directory starts empty.
    QStandardPaths::setTestModeEnabled(true);
    clearDownloads();
}

void PaddleOcrTest::effectiveTierFallsBackToBundledTiny()
{
    PaddleOcr paddle; // no Settings: the picker defaults to "small"
    QCOMPARE(paddle.tier(), QStringLiteral("small"));
    QVERIFY(!paddle.tierDownloaded());
    QVERIFY(!paddle.tierBundled());
    QVERIFY(!paddle.modelInstalled());
    // D3: nothing is downloaded, so the included tiny is what runs.
    QCOMPARE(paddle.effectiveTier(), QStringLiteral("tiny"));
    // The whole point: nothing on disk, and hasDownloads() must not pretend
    // there is (the Delete button and the "downloaded" wording hang off it).
    QVERIFY(!paddle.hasDownloads());
    QVERIFY(!QFileInfo::exists(PaddleOcr::modelDir(QStringLiteral("tiny"))));
}

void PaddleOcrTest::bundledFilesResolveFromTheBinary()
{
    // detPath/recPath/dictPath are what the loader reads, and for the bundled
    // tier they have to point at the qrc, not at a data-dir copy.
    const QList<QString> paths{PaddleOcr::detPath(QStringLiteral("tiny")),
                               PaddleOcr::recPath(QStringLiteral("tiny")),
                               PaddleOcr::dictPath(QStringLiteral("tiny"))};
    for (const QString &path : paths) {
        QVERIFY2(path.startsWith(QStringLiteral(":/paddleocr/tiny/")), qPrintable(path));
        const QFileInfo info(path);
        QVERIFY2(info.exists(), qPrintable(path));
        QVERIFY2(info.size() > 0, qPrintable(path));
    }
    // Small and Medium are not embedded: they stay data-dir paths.
    QVERIFY(PaddleOcr::detPath(QStringLiteral("small")).startsWith(
        PaddleOcr::modelDir(QStringLiteral("small"))));
}

void PaddleOcrTest::tierStatesListAllThree()
{
    PaddleOcr paddle;
    const QVariantList states = paddle.tierStates();
    QCOMPARE(states.size(), 3);
    const QStringList ids{QStringLiteral("tiny"), QStringLiteral("small"),
                          QStringLiteral("medium")};
    const QStringList wanted{QStringLiteral("included"), QStringLiteral("missing"),
                             QStringLiteral("missing")};
    for (int i = 0; i < ids.size(); ++i) {
        const QVariantMap entry = states.at(i).toMap();
        QCOMPARE(entry.value(QStringLiteral("id")).toString(), ids.at(i));
        QCOMPARE(entry.value(QStringLiteral("state")).toString(), wanted.at(i));
    }
}

void PaddleOcrTest::tinyRoutesJapaneseToTesseract()
{
    QVERIFY(!PaddleOcr::tierReadsJapanese(QStringLiteral("tiny")));

    PaddleOcr paddle;
    QCOMPARE(paddle.effectiveTier(), QStringLiteral("tiny"));
    // Readable by tiny.
    QVERIFY(paddle.supportsLanguages(QStringLiteral("eng"), false));
    QVERIFY(paddle.supportsLanguages(QStringLiteral("pol+eng"), false));
    QVERIFY(paddle.supportsLanguages(QStringLiteral("chi_sim"), false));
    QVERIFY(paddle.supportsLanguages(QStringLiteral("chi_sim+chi_tra"), false));
    // Japanese must fall back to Tesseract while tiny is the running tier.
    QVERIFY(!paddle.supportsLanguages(QStringLiteral("jpn"), false));
    QVERIFY(!paddle.supportsLanguages(QStringLiteral("jpn_vert"), false));
    QVERIFY(!paddle.supportsLanguages(QStringLiteral("chi_sim+jpn"), false));
    // Scripts PP-OCRv6 has no dictionary for stay on Tesseract at any tier.
    QVERIFY(!paddle.supportsLanguages(QStringLiteral("rus"), false));
    QVERIFY(!paddle.supportsLanguages(QStringLiteral("kor"), false));
    QVERIFY(!paddle.supportsLanguages(QStringLiteral("ara"), false));
    QVERIFY(!paddle.supportsLanguages(QStringLiteral("tha"), false));
    // Auto script mode: no OSD in PP-OCRv6, so a machine with a Japanese pack
    // installed cannot be trusted to tiny for whatever the page turns out to be.
    QVERIFY(paddle.supportsLanguages(QString(), true));
    QVERIFY(paddle.supportsLanguages(QStringLiteral("eng+pol"), true));
    QVERIFY(!paddle.supportsLanguages(QStringLiteral("eng+jpn"), true));
}

void PaddleOcrTest::smallAndMediumReadJapanese()
{
    // Without the 31 MB and 139 MB files the rule is still the same function, so
    // the table is pinned here and the download only changes which tier runs.
    for (const QString &tier : {QStringLiteral("small"), QStringLiteral("medium")}) {
        QVERIFY2(PaddleOcr::tierReadsJapanese(tier), qPrintable(tier));
        QVERIFY2(PaddleOcr::tierSupportsLanguages(tier, QStringLiteral("jpn"), false),
                 qPrintable(tier));
        QVERIFY2(PaddleOcr::tierSupportsLanguages(tier, QStringLiteral("chi_sim+jpn"), false),
                 qPrintable(tier));
        QVERIFY2(PaddleOcr::tierSupportsLanguages(tier, QStringLiteral("eng+pol"), true),
                 qPrintable(tier));
        // Still no Cyrillic, Korean, Arabic or Thai at a bigger tier: the words
        // are simply not in PP-OCRv6's dictionary.
        QVERIFY2(!PaddleOcr::tierSupportsLanguages(tier, QStringLiteral("rus"), false),
                 qPrintable(tier));
        QVERIFY2(!PaddleOcr::tierSupportsLanguages(tier, QStringLiteral("kor"), false),
                 qPrintable(tier));
    }
    // No tier means no PP-OCRv6 at all, so every spec says no.
    QVERIFY(!PaddleOcr::tierSupportsLanguages(QString(), QStringLiteral("eng"), false));
}

QTEST_MAIN(PaddleOcrTest)
#include "PaddleOcrTest.moc"
