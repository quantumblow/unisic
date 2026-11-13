#pragma once
#include <QImage>
#include <QList>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
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
// the small/medium models are one-time downloads. Tesseract stays the
// always-available engine for scripts PP-OCRv6 does not cover (Korean, Arabic,
// Cyrillic, Thai, Indic, Greek) - v6 reads Chinese (both scripts), Japanese and
// 48 Latin-script languages with one det+rec pair.
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

    // Whether PP-OCRv6 can read the requested Tesseract language spec, for the
    // tier that will actually run. No tier at all says no. Auto script mode is
    // normally accepted (the common case is mixed Chinese/Latin, which v6 reads
    // in one pass), except where the running tier cannot read what the installed
    // packs might contain: tiny has no kana, so with Japanese installed an auto
    // read stays on Tesseract. A manual spec that names a script the tier lacks
    // says no, and OcrEngine keeps Tesseract for it.
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
    QList<int> m_queue; // file indices still to fetch (-1 = the runtime archive)
    bool m_extractingRuntime = false;
    bool m_cancelled = false;
    qint64 m_doneBytes = 0;
    qint64 m_totalBytes = 0;
    qreal m_progress = 0;
    QString m_error;
    QTimer *m_idle = nullptr;
};
