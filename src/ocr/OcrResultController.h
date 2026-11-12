#pragma once
#include <QImage>
#include <QObject>
#include <QSize>
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
    Q_PROPERTY(bool translating READ translating NOTIFY translationChanged)
    Q_PROPERTY(QString translationError READ translationError NOTIFY translationChanged)
    Q_PROPERTY(bool canTranslate READ canTranslate NOTIFY translationChanged)
    Q_PROPERTY(QString translationTarget READ translationTarget NOTIFY translationChanged)

public:
    explicit OcrResultController(Settings *settings = nullptr, QObject *parent = nullptr);
    ~OcrResultController() override;

    QUrl imageSource() const { return m_source; }
    QSize imageSize() const { return m_image.size(); }
    QString text() const { return m_text; }
    QVariantList lines() const { return m_lines; }
    QImage image() const { return m_image; }
    QString translation() const { return m_translation; }
    bool translating() const { return m_translating; }
    QString translationError() const { return m_translationError; }
    bool canTranslate() const;
    QString translationTarget() const;

    // Translates the current text with the OpenAI-compatible endpoint from
    // Settings. One request at a time; a new OCR result cancels it.
    Q_INVOKABLE void translate();
    Q_INVOKABLE void cancelTranslation();

    // `words` empty is legal: a QR/barcode payload has text but no geometry.
    // The text is rebuilt from `words` when there are any, so line offsets and
    // the selectable ranges cannot drift from the picture.
    void setResult(const QImage &image, const QVector<OcrWord> &words, const QString &text);

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
    QString m_translationError;
    bool m_translating = false;
};
