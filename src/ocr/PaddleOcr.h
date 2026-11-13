#pragma once
#include <QByteArray>
#include <QImage>
#include <QList>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>
#include <functional>
#include <memory>
#include <qqmlregistration.h>

#include "OcrWord.h"

class Settings;
class QNetworkAccessManager;
class QProcess;
class QTimer;

// PP-OCRv6 (PaddleOCR 3.7's model family) on ONNX Runtime, as an optional
// second OCR engine next to Tesseract.
//
// Like "Remove background", nothing is linked: the runtime is dlopen()ed and
// the small/medium models are one-time downloads. PP-OCRv6 reads Chinese (both
// scripts), Japanese and 48 Latin-script languages with one det+rec pair; the
// other scripts use PP-OCRv5's per-script recognizers (Korean, Arabic,
// Cyrillic, Thai, Devanagari, Tamil, Telugu, Greek), one rec+dict pair each,
// while DETECTION stays the v6 DB model. Tesseract stays the always-available
// engine for everything without a downloadable recognizer.
//
// The "tiny" pair ships INSIDE the binary (a qrc built from the bytes in
// third_party/paddleocr/tiny/), so a fresh install has a working PP-OCRv6
// without a model download; small and medium stay downloads for accuracy.
// Which tier actually runs is effectiveTier(): the selected one when it is
// installed, else the bundled tiny, else nothing.
//
// Recognition runs on a worker thread; callbacks are delivered on the GUI
// thread. The loaded sessions are dropped after two minutes of idling and
// reloaded when the selected tier changes, because they are not small: about
// 0.4 GB resident for tiny, 0.6 GB for small and 1.2 GB for medium with ORT
// 1.31.1 (a 1600x1000 page: tiny 0.1 s, small 0.2 s, medium 0.55 s warm).
class PaddleOcr : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by AppContext")

    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool runtimeFound READ runtimeFound NOTIFY stateChanged)
    Q_PROPERTY(QString runtimeInfo READ runtimeInfo NOTIFY stateChanged)
    Q_PROPERTY(bool canDownloadRuntime READ canDownloadRuntime CONSTANT)
    Q_PROPERTY(bool modelInstalled READ modelInstalled NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY stateChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString downloadSizeText READ downloadSizeText NOTIFY stateChanged)
    Q_PROPERTY(bool hasDownloads READ hasDownloads NOTIFY stateChanged)
    // The selected tier, stored as Settings::paddleTier ("tiny"/"small"/"medium").
    Q_PROPERTY(QString tier READ tier NOTIFY tierChanged)
    // The tier that actually runs: "tiny"/"small"/"medium", or "" when there is
    // no PP-OCRv6 model at all. Settings shows the picker's value and this one.
    Q_PROPERTY(QString effectiveTier READ effectiveTier NOTIFY stateChanged)
    // The selected tier: shipped in the binary (only tiny) or found in the data
    // dir. Settings gates Download/Delete on these.
    Q_PROPERTY(bool tierBundled READ tierBundled NOTIFY stateChanged)
    Q_PROPERTY(bool tierDownloaded READ tierDownloaded NOTIFY stateChanged)
    // [{id: "tiny", state: "included"|"downloaded"|"missing"}, ...] for every
    // tier, so Settings can list all three states without one property each.
    Q_PROPERTY(QVariantList tierStates READ tierStates NOTIFY stateChanged)
    // [{id, name, state, bytes, sizeText, installed}, ...] for the eight
    // PP-OCRv5 script recognizers, so Settings can list all of them.
    Q_PROPERTY(QVariantList scriptStates READ scriptStates NOTIFY stateChanged)
    // The script model being downloaded, or "" when the queue is idle or
    // fetching a PP-OCRv6 tier. Settings puts the progress on that row.
    Q_PROPERTY(QString downloadingScript READ downloadingScript NOTIFY stateChanged)

public:
    using Result = std::function<void(const QString &text, const QString &error)>;
    using BoxResult = std::function<void(const QVector<OcrWord> &words, const QString &error)>;

    explicit PaddleOcr(Settings *settings = nullptr, QObject *parent = nullptr);
    ~PaddleOcr() override;

    // Linux only, like background removal: Windows has no ORT provisioning and
    // an app-local model download would be a second packaging story.
    static bool available();

    QString tier() const;
    QString effectiveTier() const;
    bool tierBundled() const;
    bool tierDownloaded() const;
    QVariantList tierStates() const;

    // PP-OCRv5 script recognizers. Detection still comes from the
    // effective PP-OCRv6 tier; only the recognizer and its dictionary are
    // per script, in <AppData>/paddleocr/scripts/<script>/.
    static QStringList scriptIds();
    static QString scriptName(const QString &script);
    static QString scriptDir(const QString &script);
    static QString scriptRecPath(const QString &script);
    static QString scriptDictPath(const QString &script);
    // The URL a script file is fetched from: the mirror layout under
    // UNISIC_PADDLE_MODEL_BASE, or the revision-pinned official URL.
    static QString scriptDownloadUrl(const QString &script, const QString &fileName);
    bool scriptInstalled(const QString &script) const;
    bool hasInstalledScripts() const;
    QVariantList scriptStates() const;
    QString downloadingScript() const { return m_downloadScript; }
    // The missing bytes of a script model; includes the shared ONNX
    // Runtime when it is not loaded yet.
    Q_INVOKABLE QString scriptDownloadSizeText(const QString &script);
    Q_INVOKABLE void downloadScript(const QString &script);
    Q_INVOKABLE void deleteScriptDownloads(const QString &script);

    // The pure routing rules, public and static so the whole table is
    // pinned by a test that needs no model file and no runtime.
    // The script a Tesseract language spec needs, or "" when one
    // recognizer cannot read the whole spec (mixed or unsupported
    // scripts, Latin-only, empty).
    static QString scriptForTesseractLanguages(const QString &langs);
    // The script a Tesseract OSD script name maps to, or "".
    static QString scriptForOsd(const QString &osdScript);
    // Whether this one script recognizer can read the whole spec.
    static bool scriptSupportsLanguages(const QString &script, const QString &langs);
    // PaddleX pred_reverse: the model emits Arabic in visual order; this
    // turns one line into logical order and keeps Latin/digit runs intact.
    static QString reverseRtlText(const QString &text);
    // The word-box form of the same rule: reverses one Arabic line in
    // place and moves `spaceBefore` with it, so text and boxes agree.
    static void reorderRtlWords(QVector<OcrWord> &words);
    // MiB the script path needs free before it starts. The v5 rec
    // replaces the v6 rec instead of adding to it, so the guard takes the
    // larger of this and tierRunMiB(), never their sum. Public and static
    // so the numbers are pinned by a test.
    static qint64 scriptRunMiB(const QString &script);
    // Parses the official ONNX package's inference.yml character_dict
    // list. Public and static so the parser is pinned by a test.
    static QVector<QString> parseDictYml(const QByteArray &raw, QString *error = nullptr);

    bool runtimeFound();
    QString runtimeInfo();
    bool canDownloadRuntime() const;
    // The SELECTED tier's three files are available, bundled or downloaded.
    bool modelInstalled() const;
    // PP-OCRv6 can run: the runtime is loaded and some tier is available.
    bool ready() { return runtimeFound() && !effectiveTier().isEmpty(); }
    bool downloading() const { return m_reply != nullptr || m_extractingRuntime; }
    qreal progress() const { return m_progress; }
    QString error() const { return m_error; }
    bool hasDownloads() const;
    QString downloadSizeText();

    // Whether PP-OCRv6 or an INSTALLED PP-OCRv5 script recognizer can read
    // the requested Tesseract language spec, for the tier that will actually
    // run. No tier at all says no. Auto script mode keeps the v6 rule here
    // (the common case is mixed Chinese/Latin, which v6 reads in one pass,
    // except where tiny cannot read kana and Japanese is installed);
    // OcrEngine runs Tesseract OSD first when a v5 script model is installed
    // and routes to it there. A manual spec that names a script neither can
    // read says no, and OcrEngine keeps Tesseract for it.
    bool supportsLanguages(const QString &langs, bool autoScript) const;
    // The pure rule behind it: can THIS tier read the spec? Public and static so
    // the whole table, Japanese on small/medium included, is pinned by a test
    // that needs no model file and no runtime.
    static bool tierSupportsLanguages(const QString &tier, const QString &langs, bool autoScript);
    // Whether a tier's recognizer can read Japanese. Only the bundled tiny one
    // cannot (its dictionary has no kana) - public and static so the routing
    // rule is pinned by a test that needs no model file.
    static bool tierReadsJapanese(const QString &tier);
    // The tier memory table (D4): MiB a run needs free before it starts. An
    // unknown id reads as the default tier, like the model table itself. Public
    // and static so the numbers are pinned by a test with no model or runtime.
    static qint64 tierRunMiB(const QString &tier);
    // MemAvailable in MiB parsed from the text of /proc/meminfo. -1 when the
    // line is absent (not Linux) or unreadable, which skips the memory guard
    // rather than refusing to run. Pure, so the parse is pinned by a test.
    static qint64 parseMemAvailableMiB(const QByteArray &meminfo);

    // Recognizes the picture; `text` is the reading-order text with one line
    // per detected text line. Skips a picture whose only content is a QR/bar
    // code and returns the code's payload instead.
    void recognize(const QImage &img, Result cb);
    // Same recognition with glyph boxes for the editor's selectable text.
    void recognizeBoxes(const QImage &img, BoxResult cb);
    // Recognition with one PP-OCRv5 script recognizer. Detection is the
    // same PP-OCRv6 model; only the rec+dict pair changes.
    void recognizeScript(const QImage &img, const QString &script, Result cb);
    void recognizeScriptBoxes(const QImage &img, const QString &script, BoxResult cb);

    // Downloads what is missing: ONNX Runtime when no usable copy exists, then
    // the det, rec and dictionary files of the selected tier when that tier is
    // not the bundled one. Each file is pinned by URL, size and SHA-256.
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancelDownload();
    // Removes the selected tier's DOWNLOADED copies. The bundled tiny files are
    // inside the binary and cannot be removed; a runtime that background
    // removal may also use is left alone.
    Q_INVOKABLE void deleteDownloads();

    // The directory a DOWNLOADED copy lives in. The bundled tiny tier is not
    // there - detPath/recPath/dictPath resolve to the embedded copy instead.
    static QString modelDir(const QString &tier);
    // The file actually read: the bundled qrc copy when there is one, else the
    // downloaded file in modelDir().
    static QString detPath(const QString &tier);
    static QString recPath(const QString &tier);
    static QString dictPath(const QString &tier);

signals:
    void stateChanged();
    void progressChanged();
    void tierChanged();

private:
    struct Impl;
    void probe();
    void fetchNext();
    void fail(const QString &message);
    void installRuntimeArchive(const QString &archive);
    QNetworkAccessManager *nam();
    // The tier shipped inside the binary, or "" if none is.
    static QString bundledTier();
    // Whether every file of `tier` is present in the binary's qrc.
    static bool bundledComplete(const QString &tier);
    // Whether every file of `tier` is present in the data dir with its pinned
    // size (a partial download does not count).
    static bool downloadedComplete(const QString &tier);
    static QString modelFilePath(const QString &tier, int index);

    Settings *m_settings;
    std::shared_ptr<Impl> m_impl;
    bool m_probed = false;
    QString m_runtimeInfo;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    QPointer<QProcess> m_tar;
    QString m_downloadTier;
    QString m_downloadScript;
    struct FetchJob {
        QString url;
        QString dest;
        qint64 bytes = 0;
        QByteArray sha256;
        bool runtime = false;
    };
    QList<FetchJob> m_queue;
    bool m_extractingRuntime = false;
    bool m_cancelled = false;
    qint64 m_doneBytes = 0;
    qint64 m_totalBytes = 0;
    qreal m_progress = 0;
    QString m_error;
    QTimer *m_idle = nullptr;
};
