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
    void scriptIdsAndStatesAreComplete();
    void scriptRoutingTable();
    void osdScriptRouting();
    void scriptDictYmlParses();
    void rtlTextReverses();
    void rtlWordsReorder();
    void scriptMemoryTableIsPinned();
    void unknownScriptHasNoPaths();
    void scriptMirrorUrl();

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

void PaddleOcrTest::scriptIdsAndStatesAreComplete()
{
    const QStringList ids = PaddleOcr::scriptIds();
    QCOMPARE(ids.size(), 8);
    QCOMPARE(ids, QStringList({QStringLiteral("korean"), QStringLiteral("arabic"),
                               QStringLiteral("cyrillic"), QStringLiteral("thai"),
                               QStringLiteral("devanagari"), QStringLiteral("tamil"),
                               QStringLiteral("telugu"), QStringLiteral("greek")}));
    PaddleOcr paddle;
    const QVariantList states = paddle.scriptStates();
    QCOMPARE(states.size(), 8);
    for (const QVariant &v : states) {
        const QVariantMap entry = v.toMap();
        QVERIFY(!entry.value(QStringLiteral("name")).toString().isEmpty());
        QVERIFY(entry.value(QStringLiteral("bytes")).toLongLong() > 0);
        // A clean data dir: every pool is missing, and none reports installed.
        QCOMPARE(entry.value(QStringLiteral("state")).toString(), QStringLiteral("missing"));
        QVERIFY(!entry.value(QStringLiteral("installed")).toBool());
        QVERIFY(!paddle.scriptInstalled(entry.value(QStringLiteral("id")).toString()));
    }
    QVERIFY(!paddle.hasInstalledScripts());
}

void PaddleOcrTest::scriptRoutingTable()
{
    using P = PaddleOcr;
    QCOMPARE(P::scriptForTesseractLanguages(QStringLiteral("kor")), QStringLiteral("korean"));
    QCOMPARE(P::scriptForTesseractLanguages(QStringLiteral("kor+eng")), QStringLiteral("korean"));
    QCOMPARE(P::scriptForTesseractLanguages(QStringLiteral("ara+fas+eng")),
             QStringLiteral("arabic"));
    QCOMPARE(P::scriptForTesseractLanguages(QStringLiteral("rus+ukr")),
             QStringLiteral("cyrillic"));
    QCOMPARE(P::scriptForTesseractLanguages(QStringLiteral("tha")), QStringLiteral("thai"));
    QCOMPARE(P::scriptForTesseractLanguages(QStringLiteral("hin+mar")),
             QStringLiteral("devanagari"));
    QCOMPARE(P::scriptForTesseractLanguages(QStringLiteral("tam")), QStringLiteral("tamil"));
    QCOMPARE(P::scriptForTesseractLanguages(QStringLiteral("tel")), QStringLiteral("telugu"));
    QCOMPARE(P::scriptForTesseractLanguages(QStringLiteral("ell")), QStringLiteral("greek"));
    QVERIFY(P::scriptForTesseractLanguages(QStringLiteral("kor+jpn")).isEmpty());
    QVERIFY(P::scriptForTesseractLanguages(QStringLiteral("kor+chi_sim")).isEmpty());
    QVERIFY(P::scriptForTesseractLanguages(QStringLiteral("kor+ara")).isEmpty());
    QVERIFY(P::scriptForTesseractLanguages(QStringLiteral("deu+kor")).isEmpty());
    QVERIFY(P::scriptForTesseractLanguages(QStringLiteral("heb")).isEmpty());
    QVERIFY(P::scriptForTesseractLanguages(QStringLiteral("ben")).isEmpty());
    QVERIFY(P::scriptForTesseractLanguages(QStringLiteral("grc")).isEmpty());
    QVERIFY(P::scriptForTesseractLanguages(QStringLiteral("kor_vert")).isEmpty());
    QVERIFY(P::scriptForTesseractLanguages(QStringLiteral("chi_sim")).isEmpty());
    QVERIFY(P::scriptForTesseractLanguages(QStringLiteral("eng")).isEmpty());
    QVERIFY(P::scriptForTesseractLanguages(QString()).isEmpty());
    QVERIFY(P::scriptSupportsLanguages(QStringLiteral("korean"), QStringLiteral("kor+eng")));
    QVERIFY(P::scriptSupportsLanguages(QStringLiteral("arabic"),
                                       QStringLiteral("ara+fas+eng")));
    QVERIFY(!P::scriptSupportsLanguages(QStringLiteral("korean"),
                                        QStringLiteral("kor+jpn")));
}

void PaddleOcrTest::osdScriptRouting()
{
    using P = PaddleOcr;
    QCOMPARE(P::scriptForOsd(QStringLiteral("Hangul")), QStringLiteral("korean"));
    QCOMPARE(P::scriptForOsd(QStringLiteral("Arabic")), QStringLiteral("arabic"));
    QCOMPARE(P::scriptForOsd(QStringLiteral("Cyrillic")), QStringLiteral("cyrillic"));
    QCOMPARE(P::scriptForOsd(QStringLiteral("Thai")), QStringLiteral("thai"));
    QCOMPARE(P::scriptForOsd(QStringLiteral("Devanagari")), QStringLiteral("devanagari"));
    QCOMPARE(P::scriptForOsd(QStringLiteral("Tamil")), QStringLiteral("tamil"));
    QCOMPARE(P::scriptForOsd(QStringLiteral("Telugu")), QStringLiteral("telugu"));
    QCOMPARE(P::scriptForOsd(QStringLiteral("Greek")), QStringLiteral("greek"));
    QVERIFY(P::scriptForOsd(QStringLiteral("Latin")).isEmpty());
    QVERIFY(P::scriptForOsd(QStringLiteral("Han")).isEmpty());
    QVERIFY(P::scriptForOsd(QStringLiteral("Japanese")).isEmpty());
    QVERIFY(P::scriptForOsd(QStringLiteral("Hangul_vert")).isEmpty());
    QVERIFY(P::scriptForOsd(QStringLiteral("Hebrew")).isEmpty());
    QVERIFY(P::scriptForOsd(QString()).isEmpty());
}

void PaddleOcrTest::scriptDictYmlParses()
{
    // The real inference.yml has a PostProcess block and a YAML list; the
    // parser must stop at the next key and unescape single quotes.
    const QByteArray yml = R"YML(Global:
  model_name: test
PostProcess:
  name: CTCLabelDecode
  character_dict:
  - '!'
  - ''''
  - \
  - ᄀ
  - '!'
Next:
  x: 1
)YML";
    QString error;
    const QVector<QString> dict = PaddleOcr::parseDictYml(yml, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(dict.size(), 5);
    QCOMPARE(dict.at(0), QStringLiteral("!"));
    QCOMPARE(dict.at(1), QStringLiteral("'"));
    QCOMPARE(dict.at(2), QStringLiteral("\\"));
    QCOMPARE(dict.at(3), QString::fromUtf8("ᄀ"));
    QCOMPARE(dict.at(4), QStringLiteral("!")); // duplicates stay: index order matters
    QString missing;
    QVERIFY(PaddleOcr::parseDictYml("Global:\n  x: 1\n", &missing).isEmpty());
    QVERIFY(!missing.isEmpty());
}

void PaddleOcrTest::rtlTextReverses()
{
    // The Arabic model emits visual order; these are the same letters reversed.
    const QString visual = QString::fromUtf8("ملاعلاب ابحرم");
    const QString logical = QString::fromUtf8("مرحبا بالعالم");
    QCOMPARE(PaddleOcr::reverseRtlText(visual), logical);
    QCOMPARE(PaddleOcr::reverseRtlText(QStringLiteral("abc 123")), QStringLiteral("abc 123"));
    QCOMPARE(PaddleOcr::reverseRtlText(QString()), QString());
}

void PaddleOcrTest::scriptMemoryTableIsPinned()
{
    // The v5 rec replaces the v6 rec; 320 MiB is the measured script path plus
    // headroom. Every id reads the same value today, but the table is per id
    // so a heavier future model is a one-line change.
    for (const QString &id : PaddleOcr::scriptIds())
        QCOMPARE(PaddleOcr::scriptRunMiB(id), 320);
    QCOMPARE(PaddleOcr::scriptRunMiB(QStringLiteral("unknown")), 320);
}

void PaddleOcrTest::unknownScriptHasNoPaths()
{
    QVERIFY(PaddleOcr::scriptDir(QStringLiteral("../korean")).isEmpty());
    QVERIFY(PaddleOcr::scriptRecPath(QStringLiteral("nope")).isEmpty());
    QVERIFY(PaddleOcr::scriptDictPath(QStringLiteral("nope")).isEmpty());
    const QString korean = PaddleOcr::scriptDir(QStringLiteral("korean"));
    QVERIFY(korean.endsWith(QStringLiteral("/paddleocr/scripts/korean")));
    QVERIFY(PaddleOcr::scriptRecPath(QStringLiteral("korean")).endsWith(
        QStringLiteral("/inference.onnx")));
    QVERIFY(PaddleOcr::scriptDictPath(QStringLiteral("korean")).endsWith(
        QStringLiteral("/inference.yml")));
}


void PaddleOcrTest::scriptMirrorUrl()
{
    qputenv("UNISIC_PADDLE_MODEL_BASE", "http://127.0.0.1:9/mirror");
    QCOMPARE(PaddleOcr::scriptDownloadUrl(QStringLiteral("korean"),
                                          QStringLiteral("inference.onnx")),
             QStringLiteral("http://127.0.0.1:9/mirror/v5/korean/inference.onnx"));
    QCOMPARE(PaddleOcr::scriptDownloadUrl(QStringLiteral("korean"),
                                          QStringLiteral("inference.yml")),
             QStringLiteral("http://127.0.0.1:9/mirror/v5/korean/inference.yml"));
    QVERIFY(PaddleOcr::scriptDownloadUrl(QStringLiteral("nope"),
                                         QStringLiteral("inference.onnx")).isEmpty());
    qunsetenv("UNISIC_PADDLE_MODEL_BASE");
    const QString official = PaddleOcr::scriptDownloadUrl(QStringLiteral("korean"),
                                                          QStringLiteral("inference.onnx"));
    QVERIFY(official.startsWith(QStringLiteral(
        "https://www.modelscope.cn/models/PaddlePaddle/korean_PP-OCRv5_mobile_rec_onnx/resolve/")));
    QVERIFY(official.endsWith(QStringLiteral("/inference.onnx")));
}

void PaddleOcrTest::rtlWordsReorder()
{
    // The recognizer emits the line left to right; the words are one glyph
    // each and a space is recorded as spaceBefore on the next glyph.
    auto wordsFrom = [](const QString &text) {
        QVector<OcrWord> words;
        bool afterSpace = false;
        int x = 0;
        for (const QChar c : text) {
            if (c == QLatin1Char(' ')) {
                afterSpace = true;
                continue;
            }
            OcrWord w;
            w.text = QString(c);
            w.rect = QRect(x, 0, 10, 20);
            w.spaceBefore = afterSpace;
            afterSpace = false;
            words.append(w);
            x += 10;
        }
        return words;
    };
    auto textFrom = [](const QVector<OcrWord> &words) {
        QString text;
        for (const OcrWord &w : words) {
            if (w.spaceBefore)
                text += QLatin1Char(' ');
            text += w.text;
        }
        return text;
    };

    QVector<OcrWord> words = wordsFrom(QString::fromUtf8("ملاعلاب ابحرم"));
    const QRect firstRect = words.first().rect;
    PaddleOcr::reorderRtlWords(words);
    QCOMPARE(textFrom(words), QString::fromUtf8("مرحبا بالعالم"));
    // The box travels with its glyph: the logical first word is the visual
    // last one, not the old leftmost box.
    QCOMPARE(words.first().rect, QRect(110, 0, 10, 20));
    QCOMPARE(words.last().rect, firstRect);

    QVector<OcrWord> mixed = wordsFrom(QString::fromUtf8("ملاعلاب abc"));
    PaddleOcr::reorderRtlWords(mixed);
    QCOMPARE(textFrom(mixed), QString::fromUtf8("abc بالعالم"));
}
QTEST_MAIN(PaddleOcrTest)
#include "PaddleOcrTest.moc"
