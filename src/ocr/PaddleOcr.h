#pragma once
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>
#include <qqmlregistration.h>

#include "OcrWord.h"

class Settings;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTimer;

// PP-OCRv6 (PaddleOCR 3.7's model family) on ONNX Runtime, as an optional
// second OCR engine next to Tesseract.
//
// Like "Remove background", nothing is linked: the runtime is dlopen()ed and
// the models are one-time downloads, so every build has the code and the
// feature only waits for files. Tesseract stays the always-available engine
// for scripts PP-OCRv6 does not cover (Korean, Arabic, Cyrillic, Thai, Indic,
// Greek) - v6 reads Chinese (both scripts), Japanese and 48 Latin-script
// languages with one det+rec pair.
//
// Recognition runs on a worker thread; callbacks are delivered on the GUI
// thread. The loaded sessions are dropped after a minute of idling so an idle
// Unisic does not keep ~60 MB of ORT arenas around.
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

public:
    using Result = std::function<void(const QString &text, const QString &error)>;
    using BoxResult = std::function<void(const QVector<OcrWord> &words, const QString &error)>;

    explicit PaddleOcr(Settings *settings = nullptr, QObject *parent = nullptr);
    ~PaddleOcr() override;

    // Linux only, like background removal: Windows has no ORT provisioning and
    // an app-local model download would be a second packaging story.
    static bool available();

    QString tier() const;

    bool runtimeFound();
    QString runtimeInfo();
    bool canDownloadRuntime() const;
    bool modelInstalled() const;
    bool ready() { return runtimeFound() && modelInstalled(); }
    bool downloading() const { return m_reply != nullptr || m_extractingRuntime; }
    qreal progress() const { return m_progress; }
    QString error() const { return m_error; }
    bool hasDownloads() const;
    QString downloadSizeText();

    // Whether PP-OCRv6 can read the requested Tesseract language spec. Auto
    // script mode is accepted (the common case is mixed Chinese/Latin, which
    // v6 reads in one pass); a manual spec that names a script v6 lacks says
    // no, and OcrEngine keeps Tesseract for it.
    bool supportsLanguages(const QString &langs, bool autoScript) const;

    // Recognizes the picture; `text` is the reading-order text with one line
    // per detected text line. Skips a picture whose only content is a QR/bar
    // code and returns the code's payload instead.
    void recognize(const QImage &img, Result cb);
    // Same recognition with glyph boxes for the editor's selectable text.
    void recognizeBoxes(const QImage &img, BoxResult cb);

    // Downloads what is missing: ONNX Runtime when no usable copy exists, then
    // the det, rec and dictionary files of the selected tier. Each file is
    // pinned by URL, size and SHA-256.
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancelDownload();
    // Removes the selected tier's model files. A runtime that background
    // removal may also use is left alone.
    Q_INVOKABLE void deleteDownloads();

    static QString modelDir(const QString &tier);
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
