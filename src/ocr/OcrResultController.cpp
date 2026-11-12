#include "OcrResultController.h"

#include "Settings.h"
#include "Translator.h"

#include <QDir>
#include <QHash>
#include <QTemporaryFile>
#include <cmath>

namespace {

// A screenshot can carry transparency; a cover that keeps it would show the
// original text through the translation. Blend onto white, the same paper the
// OCR pre-processing assumes.
QRgb opaque(QRgb pixel)
{
    const int alpha = qAlpha(pixel);
    if (alpha == 255)
        return pixel;
    const auto blend = [alpha](int channel) { return (channel * alpha + 255 * (255 - alpha)) / 255; };
    return qRgb(blend(qRed(pixel)), blend(qGreen(pixel)), blend(qBlue(pixel)));
}

QRgb bucketKey(QRgb pixel)
{
    return qRgb(qRed(pixel) & 0xF8, qGreen(pixel) & 0xF8, qBlue(pixel) & 0xF8);
}

struct Bucket {
    int count = 0;
    qint64 r = 0;
    qint64 g = 0;
    qint64 b = 0;
    void add(QRgb pixel)
    {
        ++count;
        r += qRed(pixel);
        g += qGreen(pixel);
        b += qBlue(pixel);
    }
};

// Mean colour of the most common bucket: a page is one colour plus glyphs, so
// the mode is the paper and the average inside it removes the quantization.
QColor dominantColor(const QHash<QRgb, Bucket> &histogram, const QColor &fallback)
{
    const Bucket *best = nullptr;
    QRgb bestKey = 0;
    for (auto it = histogram.constBegin(); it != histogram.constEnd(); ++it) {
        if (!best || it.value().count > best->count
            || (it.value().count == best->count && it.key() < bestKey)) {
            best = &it.value();
            bestKey = it.key();
        }
    }
    if (!best || best->count == 0)
        return fallback;
    return QColor(int(best->r / best->count), int(best->g / best->count),
                  int(best->b / best->count));
}

double linearChannel(double value)
{
    value /= 255.0;
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor &color)
{
    return 0.2126 * linearChannel(color.red()) + 0.7152 * linearChannel(color.green())
           + 0.0722 * linearChannel(color.blue());
}

double contrastRatio(const QColor &a, const QColor &b)
{
    const double first = relativeLuminance(a);
    const double second = relativeLuminance(b);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

int colorDistance(QRgb a, QRgb b)
{
    return std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b))
           + std::abs(qBlue(a) - qBlue(b));
}

} // namespace

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
    m_translation.clear();
    m_translatedLines.clear();
    m_translationUnchanged = false;
    emit translationChanged();

    const quint64 seq = ++m_translationSeq;
    m_chunks.clear();
    m_translatedChunks.clear();
    m_chunkIndex = 0;
    if (m_lines.size() > 1) {
        // Numbered requests, one per chunk, so every translation comes back
        // with the line box it belongs to. A model that merges or splits lines
        // gives a count mismatch and the whole-text request takes over.
        QStringList texts;
        texts.reserve(m_lines.size());
        for (const QVariant &value : m_lines)
            texts.append(value.toMap().value(QStringLiteral("text")).toString());
        m_chunks = chunkLines(texts);
        requestNextLineChunk(seq);
        return;
    }
    requestWholeText(seq);
}

void OcrResultController::requestNextLineChunk(quint64 seq)
{
    if (seq != m_translationSeq)
        return;
    if (m_chunkIndex >= m_chunks.size()) {
        QStringList translated;
        for (const QStringList &chunk : m_translatedChunks)
            translated += chunk;
        QString text;
        m_translatedLines = translatedLineMap(m_lines, translated, m_image, &text);
        m_translation = text;
        m_translationUnchanged = translationLooksUnchanged(m_text, m_translation);
        m_translating = false;
        emit translationChanged();
        if (!m_translation.isEmpty())
            emit translationReady();
        return;
    }
    const QStringList chunk = m_chunks.at(m_chunkIndex);
    m_translator->translateWith(
        Translator::numberedPrompt(translationTarget()), Translator::numberedRequest(chunk),
        [this, seq, chunk](const QString &body, const QString &error) {
            if (seq != m_translationSeq)
                return;
            if (!error.isEmpty()) {
                failTranslation(seq, error);
                return;
            }
            QString parseError;
            const QStringList parsed = Translator::parseNumberedLines(body, chunk.size(), &parseError);
            if (parsed.size() != chunk.size()) {
                // The service answered, but not line by line: the translation is
                // still worth showing, just in the pane instead of on the picture.
                m_translatedChunks.clear();
                requestWholeText(seq);
                return;
            }
            m_translatedChunks.append(parsed);
            ++m_chunkIndex;
            requestNextLineChunk(seq);
        });
}

void OcrResultController::requestWholeText(quint64 seq)
{
    if (seq != m_translationSeq)
        return;
    m_translator->translate(m_text, translationTarget(),
                            [this, seq](const QString &text, const QString &error) {
        if (seq != m_translationSeq)
            return;
        m_translating = false;
        if (!error.isEmpty()) {
            m_translationError = error;
            emit translationChanged();
            return;
        }
        m_translation = text;
        m_translationError.clear();
        m_translatedLines.clear();
        // The whole-text answer may still be one line per input line (the
        // service ignored the numbering but translated line by line): map it
        // back onto the picture with the same tolerant parser instead of
        // leaving the covers empty.
        if (m_lines.size() > 1) {
            const QStringList rows =
                Translator::parseNumberedLines(text, int(m_lines.size()), nullptr);
            if (rows.size() == m_lines.size()) {
                QString joined;
                const QVariantList map = translatedLineMap(m_lines, rows, m_image, &joined);
                m_translation = joined;
                m_translatedLines = map;
            }
        }
        m_translationUnchanged = translationLooksUnchanged(m_text, m_translation);
        if (m_lines.size() == 1) {
            // One line box: the whole translation is that line's translation,
            // so the overlay works without a numbered round trip.
            QString joined;
            const QVariantList map =
                translatedLineMap(m_lines, QStringList{text}, m_image, &joined);
            if (joined == text)
                m_translatedLines = map;
        }
        emit translationChanged();
        if (!m_translation.isEmpty())
            emit translationReady();
    });
}

void OcrResultController::failTranslation(quint64 seq, const QString &error)
{
    if (seq != m_translationSeq)
        return;
    m_translating = false;
    m_translationError = error;
    emit translationChanged();
}

void OcrResultController::cancelTranslation()
{
    // Bump the generation first: a reply that lands after this must not touch
    // the new state.
    ++m_translationSeq;
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
    ++m_translationSeq;
    if (m_translator)
        m_translator->cancel();
    m_translation.clear();
    m_translatedLines.clear();
    m_translationUnchanged = false;
    m_chunks.clear();
    m_translatedChunks.clear();
    m_chunkIndex = 0;
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

OcrResultController::LineColors OcrResultController::sampleLineColors(const QImage &image,
                                                                    const QRect &rect)
{
    LineColors colors{QColor(255, 255, 255), QColor(0, 0, 0)};
    if (image.isNull() || rect.isEmpty())
        return colors;
    QImage source = image;
    if (source.format() != QImage::Format_ARGB32 && source.format() != QImage::Format_RGB32)
        source = source.convertToFormat(QImage::Format_ARGB32);
    const QRect tight = rect.intersected(source.rect());
    if (tight.isEmpty())
        return colors;

    const int step = std::max(1, int(std::sqrt(double(tight.width()) * tight.height() / 12000.0)));

    // The box holds the page between the glyphs, so its own mode is the colour
    // the cover has to match - which is what makes a line on a coloured banner
    // get the banner colour and a plain line get the paper colour.
    QHash<QRgb, Bucket> inside;
    qint64 insidePixels = 0;
    for (int y = tight.top(); y <= tight.bottom(); y += step) {
        const QRgb *row = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = tight.left(); x <= tight.right(); x += step) {
            const QRgb pixel = opaque(row[x]);
            inside[bucketKey(pixel)].add(pixel);
            ++insidePixels;
        }
    }
    QColor background = dominantColor(inside, QColor(255, 255, 255));

    // The glyphs themselves are everything in the box that is not the page.
    QHash<QRgb, Bucket> ink;
    qint64 inkPixels = 0;
    for (int y = tight.top(); y <= tight.bottom(); y += step) {
        const QRgb *row = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = tight.left(); x <= tight.right(); x += step) {
            const QRgb pixel = opaque(row[x]);
            if (colorDistance(pixel, background.rgb()) < 40)
                continue;
            ink[bucketKey(pixel)].add(pixel);
            ++inkPixels;
        }
    }
    if (inkPixels * 5 > insidePixels * 3) {
        // More than half the box is "not the page": a picture, a gradient or a
        // photo behind the text, where the mode means nothing. Match the page
        // around the box instead and let the contrast guard choose the ink.
        const int margin = std::max(3, tight.height() / 2);
        const QRect ring = tight.adjusted(-margin, -margin, margin, margin).intersected(source.rect());
        QHash<QRgb, Bucket> around;
        for (int y = ring.top(); y <= ring.bottom(); y += step) {
            const QRgb *row = reinterpret_cast<const QRgb *>(source.constScanLine(y));
            for (int x = ring.left(); x <= ring.right(); x += step) {
                if (tight.contains(x, y))
                    continue;
                around[bucketKey(opaque(row[x]))].add(opaque(row[x]));
            }
        }
        if (!around.isEmpty())
            background = dominantColor(around, background);
    }
    const QColor sampled = dominantColor(ink, background.lightnessF() > 0.5f
                                                  ? QColor(0, 0, 0)
                                                  : QColor(255, 255, 255));
    // Keep the original ink colour when it is readable on the page, and a plain
    // black or white when it is not (a photo, a gradient, a highlighted line).
    colors.background = background;
    colors.foreground = contrastRatio(background, sampled) >= 3.0
                            ? sampled
                            : (relativeLuminance(background) > 0.45 ? QColor(0, 0, 0)
                                                                    : QColor(255, 255, 255));
    return colors;
}

bool OcrResultController::translationLooksUnchanged(const QString &source,
                                                    const QString &translation)
{
    const auto squash = [](const QString &text) {
        QString out;
        out.reserve(text.size());
        for (const QChar c : text) {
            if (!c.isSpace())
                out.append(c);
        }
        return out;
    };
    const QString from = squash(source);
    return !from.isEmpty() && from == squash(translation);
}

QVector<QStringList> OcrResultController::chunkLines(const QStringList &lines, int perRequest)
{
    QVector<QStringList> chunks;
    if (perRequest < 1)
        perRequest = 1;
    for (int i = 0; i < lines.size(); i += perRequest)
        chunks.append(lines.mid(i, perRequest));
    return chunks;
}

QVariantList OcrResultController::translatedLineMap(const QVariantList &lines,
                                                    const QStringList &translated,
                                                    const QImage &image, QString *text)
{
    // Convert once: sampleLineColors() would otherwise deep-copy a 4K capture
    // for every line box.
    QImage source = image;
    if (!source.isNull() && source.format() != QImage::Format_ARGB32
        && source.format() != QImage::Format_RGB32)
        source = source.convertToFormat(QImage::Format_ARGB32);
    QVariantList map;
    QString joined;
    for (int i = 0; i < lines.size(); ++i) {
        const QVariantMap line = lines.at(i).toMap();
        const QRect rect(line.value(QStringLiteral("x")).toInt(),
                         line.value(QStringLiteral("y")).toInt(),
                         line.value(QStringLiteral("width")).toInt(),
                         line.value(QStringLiteral("height")).toInt());
        const QString value = i < translated.size() ? translated.at(i) : QString();
        if (i)
            joined += QLatin1Char('\n');
        // The OCR box cuts through the outer glyphs (a wider letter pokes past
        // it), so the cover grows a little - more sideways than up and down, so
        // it reaches the last letter without eating the line above.
        const int padX = std::clamp(int(std::lround(rect.height() * 0.3)), 3, 32);
        const int padY = 1;
        const QRect cover = rect.adjusted(-padX, -padY, padX, padY);
        const LineColors colors = sampleLineColors(source, cover);
        QVariantMap entry;
        entry.insert(QStringLiteral("text"), value);
        entry.insert(QStringLiteral("start"), int(joined.size()));
        entry.insert(QStringLiteral("length"), value.size());
        entry.insert(QStringLiteral("x"), cover.x());
        entry.insert(QStringLiteral("y"), cover.y());
        entry.insert(QStringLiteral("width"), cover.width());
        entry.insert(QStringLiteral("height"), cover.height());
        entry.insert(QStringLiteral("background"), colors.background.name(QColor::HexRgb));
        entry.insert(QStringLiteral("foreground"), colors.foreground.name(QColor::HexRgb));
        map.append(entry);
        joined += value;
    }
    if (text)
        *text = joined;
    return map;
}
