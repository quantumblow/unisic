#include "OcrResultController.h"

#include "Settings.h"
#include "Translator.h"

#include <QDir>
#include <QTemporaryFile>

OcrResultController::OcrResultController(Settings *settings, QObject *parent)
    : QObject(parent), m_settings(settings), m_translator(new Translator(settings, this))
{
    if (m_settings) {
        for (auto signal : {&Settings::translationEndpointChanged, &Settings::translationModelChanged,
                            &Settings::translationApiKeyChanged, &Settings::translationTargetChanged})
            connect(m_settings, signal, this, &OcrResultController::translationChanged);
    }
}

OcrResultController::~OcrResultController()
{
    if (m_translator)
        m_translator->cancel();
}

bool OcrResultController::canTranslate() const
{
    return m_translator && m_translator->configured() && !m_text.isEmpty();
}

QString OcrResultController::translationTarget() const
{
    if (!m_settings)
        return QStringLiteral("English");
    const QString target = m_settings->translationTarget().trimmed();
    return target.isEmpty() ? QStringLiteral("English") : target;
}

void OcrResultController::translate()
{
    if (m_text.isEmpty()) {
        m_translationError = tr("No text to translate");
        emit translationChanged();
        return;
    }
    if (!canTranslate()) {
        m_translationError = tr("Set the translation endpoint and model in Settings > General");
        emit translationChanged();
        return;
    }
    if (m_translating)
        return;
    m_translating = true;
    m_translationError.clear();
    emit translationChanged();
    const QString target = translationTarget();
    m_translator->translate(m_text, target, [this](const QString &text, const QString &error) {
        m_translating = false;
        if (!error.isEmpty()) {
            m_translationError = error;
        } else {
            m_translation = text;
            m_translationError.clear();
        }
        emit translationChanged();
        if (!m_translation.isEmpty() && m_translationError.isEmpty())
            emit translationReady();
    });
}

void OcrResultController::cancelTranslation()
{
    if (m_translator)
        m_translator->cancel();
    if (m_translating) {
        m_translating = false;
        emit translationChanged();
    }
}

void OcrResultController::setResult(const QImage &image, const QVector<OcrWord> &words,
                                    const QString &text)
{
    if (m_translator)
        m_translator->cancel();
    m_translation.clear();
    m_translationError.clear();
    m_translating = false;
    m_image = image;
    m_lines.clear();
    if (words.isEmpty()) {
        m_text = text;
    } else {
        // Rebuild the text from the glyphs so every line's [start, length]
        // range indexes the exact string the pane shows. The caller's text is
        // only used for payloads that have no glyph geometry (QR/barcodes).
        QString built;
        QString lineText;
        QRect lineRect;
        int currentLine = -1;
        const auto flush = [&] {
            if (currentLine < 0 || lineText.isEmpty())
                return;
            if (!built.isEmpty())
                built += QLatin1Char('\n');
            const int start = int(built.size());
            built += lineText;
            QVariantMap line;
            line.insert(QStringLiteral("text"), lineText);
            line.insert(QStringLiteral("x"), lineRect.x());
            line.insert(QStringLiteral("y"), lineRect.y());
            line.insert(QStringLiteral("width"), lineRect.width());
            line.insert(QStringLiteral("height"), lineRect.height());
            line.insert(QStringLiteral("start"), start);
            line.insert(QStringLiteral("length"), lineText.size());
            m_lines.append(line);
        };
        for (const OcrWord &w : words) {
            if (w.line != currentLine) {
                flush();
                currentLine = w.line;
                lineText.clear();
                lineRect = w.rect;
            }
            if (!lineText.isEmpty() && w.spaceBefore)
                lineText += QLatin1Char(' ');
            lineText += w.text;
            lineRect = lineRect.united(w.rect);
        }
        flush();
        m_text = built;
    }

    // QML's Image reads a real file. The query string makes a second result
    // replace the first even when the path is reused by the temporary file.
    m_file.reset();
    m_source.clear();
    if (!image.isNull()) {
        m_file = std::make_unique<QTemporaryFile>(
            QDir::tempPath() + QStringLiteral("/unisic-ocr-XXXXXX.png"));
        if (m_file->open()) {
            if (image.save(m_file.get(), "PNG")) {
                m_file->close();
                QUrl url = QUrl::fromLocalFile(m_file->fileName());
                url.setQuery(QStringLiteral("v=%1").arg(++m_revision));
                m_source = url;
            } else {
                m_file.reset();
            }
        }
    }
    emit resultChanged();
    emit translationChanged();
}
