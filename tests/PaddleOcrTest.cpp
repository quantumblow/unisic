#include <QtTest>
#include <QDir>
#include <QFile>
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
    void tierMemoryTableIsPinned();
    void memAvailableParsesProcText();
    void selectedTierInstalledWinsOverBundledTiny();

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

void PaddleOcrTest::tierMemoryTableIsPinned()
{
    // D4's numbers, now a pure function instead of a file-static. They are what
    // refuses a Medium run on a small machine before ORT gets to fail opaquely.
    QCOMPARE(PaddleOcr::tierRunMiB(QStringLiteral("tiny")), qint64(700));
    QCOMPARE(PaddleOcr::tierRunMiB(QStringLiteral("small")), qint64(1000));
    QCOMPARE(PaddleOcr::tierRunMiB(QStringLiteral("medium")), qint64(1800));
    // Unknown or corrupt settings run the default tier, exactly like tierPin().
    QCOMPARE(PaddleOcr::tierRunMiB(QString()), qint64(1000));
    QCOMPARE(PaddleOcr::tierRunMiB(QStringLiteral("large")), qint64(1000));
}

void PaddleOcrTest::memAvailableParsesProcText()
{
    const QByteArray sample =
        "MemTotal:       32768000 kB\n"
        "MemFree:         1234567 kB\n"
        "MemAvailable:   16384000 kB\n"
        "Buffers:           12345 kB\n";
    QCOMPARE(PaddleOcr::parseMemAvailableMiB(sample), qint64(16000));
    // No line at all: -1 skips the guard instead of reading as zero memory.
    QCOMPARE(PaddleOcr::parseMemAvailableMiB("MemTotal: 1024 kB\n"), qint64(-1));
    QCOMPARE(PaddleOcr::parseMemAvailableMiB(QByteArray()), qint64(-1));
    // Unreadable values are "unknown", never "nothing free".
    QCOMPARE(PaddleOcr::parseMemAvailableMiB("MemAvailable: plenty\n"), qint64(-1));
    QCOMPARE(PaddleOcr::parseMemAvailableMiB("MemAvailable: 0 kB\n"), qint64(-1));
    // The field may follow the colon with no space at all.
    QCOMPARE(PaddleOcr::parseMemAvailableMiB("MemAvailable:4096 kB\n"), qint64(4));
}

void PaddleOcrTest::selectedTierInstalledWinsOverBundledTiny()
{
    // downloadedComplete() asks for the pinned size on disk, so a sparse file of
    // exactly that size stands in for the 31 MB download. The sizes must stay in
    // step with the kTiers table in src/ocr/PaddleOcr.cpp.
    struct Fixture { const char *name; qint64 bytes; };
    const Fixture files[] = {
        {"PP-OCRv6_det_small.onnx", 9929594},
        {"PP-OCRv6_rec_small.onnx", 21234383},
        {"ppocrv6_dict.txt", 74947},
    };
    const QString dir = PaddleOcr::modelDir(QStringLiteral("small"));
    QVERIFY(QDir().mkpath(dir));
    for (const Fixture &f : files) {
        QFile file(dir + QLatin1Char('/') + QLatin1String(f.name));
        QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.fileName()));
        QVERIFY2(file.resize(f.bytes), qPrintable(file.fileName()));
        file.close();
    }

    PaddleOcr paddle;
    QCOMPARE(paddle.tier(), QStringLiteral("small"));
    QVERIFY(paddle.tierDownloaded());
    QVERIFY(paddle.modelInstalled());
    // The branch under test: an installed selected tier beats the bundled tiny.
    QCOMPARE(paddle.effectiveTier(), QStringLiteral("small"));
    // Routing follows that tier with no runtime loaded, so Japanese is PP-OCRv6's.
    QVERIFY(paddle.supportsLanguages(QStringLiteral("jpn"), false));

    clearDownloads();
    PaddleOcr fresh;
    QCOMPARE(fresh.effectiveTier(), QStringLiteral("tiny"));
    QVERIFY(!fresh.tierDownloaded());
}

QTEST_MAIN(PaddleOcrTest)
#include "PaddleOcrTest.moc"
