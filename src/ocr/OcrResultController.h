#pragma once
#include <QColor>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVector>
#include <qqmlregistration.h>

#include "OcrWord.h"

#include <memory>

class QTemporaryFile;
class Settings;
class Translator;

// The data behind the OCR result window: the untouched screenshot, the full
// recognized text, and one selectable line box per OCR text line. The window
// keeps the source image visible next to (or below) a real text pane, so the
// mouse can select and copy exactly like a text document, and the line boxes
// on the picture select the matching range in that pane.
//
// It owns a temporary PNG for QML's Image; the file lives until the next
// result or until the app quits. No OCR work happens here.
class OcrResultController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by AppContext")

    Q_PROPERTY(QUrl imageSource READ imageSource NOTIFY resultChanged)
    Q_PROPERTY(QSize imageSize READ imageSize NOTIFY resultChanged)
    Q_PROPERTY(QString text READ text NOTIFY resultChanged)
    // { text, x, y, width, height, start, length } in image pixels; start and
    // length index into `text`, so a click can select the line in the pane.
    Q_PROPERTY(QVariantList lines READ lines NOTIFY resultChanged)
    // OpenAI-compatible translation of `text`; empty until Translate is pressed.
    Q_PROPERTY(QString translation READ translation NOTIFY translationChanged)
    // The same translation split back onto the line boxes, so the picture can
    // show it in place of the original text: { text, x, y, width, height,
    // start, length, background, foreground }. Empty when the picture has no
    // line geometry (a QR payload) or when the service did not keep the line
    // numbering; the pane then shows the whole text instead.
    Q_PROPERTY(QVariantList translatedLines READ translatedLines NOTIFY translationChanged)
    Q_PROPERTY(bool translating READ translating NOTIFY translationChanged)
    Q_PROPERTY(QString translationError READ translationError NOTIFY translationChanged)
    Q_PROPERTY(bool canTranslate READ canTranslate NOTIFY translationChanged)
    Q_PROPERTY(QString translationTarget READ translationTarget NOTIFY translationChanged)
    // True when a successful answer is the recognized text itself (a service
    // that echoed the input, often for a language pair it handles badly). The
    // window says so instead of presenting the source as a translation.
    Q_PROPERTY(bool translationUnchanged READ translationUnchanged NOTIFY translationChanged)

public:
    explicit OcrResultController(Settings *settings = nullptr, QObject *parent = nullptr);
    ~OcrResultController() override;

    QUrl imageSource() const { return m_source; }
    QSize imageSize() const { return m_image.size(); }
    QString text() const { return m_text; }
    QVariantList lines() const { return m_lines; }
    QImage image() const { return m_image; }
    QString translation() const { return m_translation; }
    QVariantList translatedLines() const { return m_translatedLines; }
    bool translating() const { return m_translating; }
    QString translationError() const { return m_translationError; }
    bool canTranslate() const;
    QString translationTarget() const;
    bool translationUnchanged() const { return m_translationUnchanged; }

    // Translates the current text with the OpenAI-compatible endpoint from
    // Settings. One request at a time; a new OCR result cancels it.
    Q_INVOKABLE void translate();
    Q_INVOKABLE void cancelTranslation();

    // `words` empty is legal: a QR/barcode payload has text but no geometry.
    // The text is rebuilt from `words` when there are any, so line offsets and
    // the selectable ranges cannot drift from the picture.
    void setResult(const QImage &image, const QVector<OcrWord> &words, const QString &text);

    // Colours for covering one line box: the background is the mode of the box
    // interior (the page between the glyphs) and the text colour the mode of
    // the pixels that differ from it, so the cover blends into the page - and a
    // line on a coloured banner takes the banner colour. Falls back to black or
    // white when the sampled pair is unreadable together.
    struct LineColors {
        QColor background;
        QColor foreground;
    };
    static LineColors sampleLineColors(const QImage &image, const QRect &rect);
    // Lays `translated` (one entry per line box) onto `lines`: the joined text
    // goes to `text`, and every entry carries the box geometry plus its
    // [start, length) range in that text, so clicking a translated line on the
    // picture selects exactly its translation in the pane.
    static QVariantList translatedLineMap(const QVariantList &lines, const QStringList &translated,
                                          const QImage &image, QString *text);
    // Comparing with whitespace squashed: a service that echoes the input often
    // re-wraps or re-spaces it, and that is still "unchanged" to the reader.
    static bool translationLooksUnchanged(const QString &source, const QString &translation);
    // A page can be hundreds of lines; one request per this many keeps the
    // prompt (and the cost) bounded and still leaves every answer mappable.
    static QVector<QStringList> chunkLines(const QStringList &lines, int perRequest = 40);

signals:
    void resultChanged();
    void translationChanged();
    // A new translation finished; the window switches to the Translation view.
    void translationReady();

private:
    QImage m_image;
    QUrl m_source;
    QString m_text;
    QVariantList m_lines;
    std::unique_ptr<QTemporaryFile> m_file;
    quint64 m_revision = 0;
    Settings *m_settings = nullptr;
    Translator *m_translator = nullptr;
    QString m_translation;
    QVariantList m_translatedLines;
    bool m_translationUnchanged = false;
    QString m_translationError;
    bool m_translating = false;
    // A line-by-line translation runs one numbered request per chunk; the
    // generation bumps on cancel or a new result and makes every late callback
    // a no-op.
    quint64 m_translationSeq = 0;
    QVector<QStringList> m_chunks;
    QVector<QStringList> m_translatedChunks;
    int m_chunkIndex = 0;

    void requestNextLineChunk(quint64 seq);
    void requestWholeText(quint64 seq);
    void failTranslation(quint64 seq, const QString &error);
};
