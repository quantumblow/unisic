#include "OcrResultController.h"

#include "Settings.h"
#include "Translator.h"

#include <QDir>
#include <algorithm>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QTemporaryFile>
#include <QTextLayout>
#include <QTextOption>
#include <cmath>

namespace {

constexpr int kChunkRetryDelayMs = 700;

// QML's Text with wrapMode Text.Wrap: break at word boundaries, and anywhere
// when a word is longer than the line. The fit and the drawing share this
// function, so a saved cover breaks exactly where the window's does.
void layOutText(QTextLayout &layout, int width, bool rtl)
{
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setAlignment(rtl ? Qt::AlignRight : Qt::AlignLeft);
    layout.setTextOption(option);
    layout.beginLayout();
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
    }
    layout.endLayout();
}

int laidOutHeight(const QTextLayout &layout)
{
    return layout.lineCount() * QFontMetrics(layout.font()).lineSpacing();
}

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

// The exact string the pane shows for a set of line boxes. m_text is built the
// same way, so translating all lines and translating the same lines as a
// selection produce the same source text.
QString textOfLines(const QVariantList &lines)
{
    QStringList texts;
    texts.reserve(lines.size());
    for (const QVariant &value : lines)
        texts.append(value.toMap().value(QStringLiteral("text")).toString());
    return texts.join(QLatin1Char('\n'));
}

// A CJK glyph fills nearly the whole em box, while a Latin one only its cap
// height, so a cover fitted to the line box reads noticeably larger than the
// text it replaces. Such covers are set 10% below the largest size that fits;
// the saved composite reads the same `fontSize`, so both stay in step.
constexpr double kCjkCoverSizeScale = 0.9;

bool hasCjkGlyphs(const QString &text)
{
    for (const QChar c : text) {
        const ushort u = c.unicode();
        // CJK radicals, kana and unified ideographs; Hangul syllables;
        // compatibility ideographs. Surrogates (Ext B and beyond) are rare in
        // a cover and fall back to the normal fit.
        if ((u >= 0x2E80 && u <= 0x9FFF) || (u >= 0xAC00 && u <= 0xD7AF)
            || (u >= 0xF900 && u <= 0xFAFF))
            return true;
    }
    return false;
}

// The overshoot a cover grows over its box, stopped halfway into the gutter
// with a neighbour on the same row (M3): the OCR box cuts through the outer
// glyphs, so the cover reaches them without eating the line above or the first
// letter of the next column. `skipIndex` is the box the cover is drawn for; a
// partial fragment passes the line box it sits in, so the fragment's own line
// never counts as its neighbour.
QRect paddedCoverRect(const QRect &rect, const QVector<QRect> &boxes, int skipIndex)
{
    const int padX = std::clamp(int(std::lround(rect.height() * 0.3)), 3, 32);
    const int padY = 1;
    int leftPad = padX;
    int rightPad = padX;
    for (int j = 0; j < boxes.size(); ++j) {
        if (j == skipIndex)
            continue;
        const QRect &otherRect = boxes.at(j);
        // Same visual row only: the box above or below is not a column
        // neighbour, and its overshoot does not matter here.
        if (otherRect.top() > rect.bottom() || otherRect.bottom() < rect.top())
            continue;
        // At most half the blank gutter, so two covers can never reach each
        // other: each stops in the middle of the space between the boxes
        // instead of both growing the full 30% into it.
        if (otherRect.right() < rect.left()) {
            const int gap = rect.left() - otherRect.right() - 1;
            leftPad = std::min(leftPad, std::max(0, gap / 2));
        } else if (otherRect.left() > rect.right()) {
            const int gap = otherRect.left() - rect.right() - 1;
            rightPad = std::min(rightPad, std::max(0, gap / 2));
        }
    }
    return rect.adjusted(-leftPad, -padY, rightPad, padY);
}

// Line boxes are not the only thing a cover must not paint over. On one line,
// a word cover's 30% overshoot can reach into the word before it; a line cover
// next to a label that the OCR kept in another box can do the same. Stop the
// left/right pad halfway into the gutter to the nearest glyph OUTSIDE the
// covered [start, start+length) range, on the same visual row. Glyphs inside
// the range are the ones being replaced, so they are not limits.
void capCoverAtGlyphs(const QVector<OcrResultController::Glyph> &glyphs, int start, int length,
                      const QRect &box, QRect *cover)
{
    if (!cover || length <= 0 || glyphs.isEmpty())
        return;
    const int end = start + length;
    int leftGap = -1;
    int rightGap = -1;
    for (const OcrResultController::Glyph &glyph : glyphs) {
        if (glyph.rect.top() > box.bottom() || glyph.rect.bottom() < box.top())
            continue;
        if (glyph.start + glyph.length <= start) {
            const int gap = box.left() - glyph.rect.right() - 1;
            if (leftGap < 0 || gap < leftGap)
                leftGap = gap;
        } else if (glyph.start >= end) {
            const int gap = glyph.rect.left() - box.right() - 1;
            if (rightGap < 0 || gap < rightGap)
                rightGap = gap;
        }
    }
    if (leftGap >= 0) {
        const int pad = std::min(box.left() - cover->left(), std::max(0, leftGap / 2));
        cover->setLeft(box.left() - pad);
    }
    if (rightGap >= 0) {
        const int pad = std::min(cover->right() - box.right(), std::max(0, rightGap / 2));
        cover->setRight(box.right() + pad);
    }
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
    // The window shows the line and fades it; the timing lives here so every
    // caller (C++ actions and QML copy buttons) behaves the same.
    m_statusTimer.setSingleShot(true);
    m_statusTimer.setInterval(2500);
    connect(&m_statusTimer, &QTimer::timeout, this, [this] {
        if (m_statusText.isEmpty())
            return;
        m_statusText.clear();
        m_statusIsError = false;
        emit statusTextChanged();
    });
    // Streaming tokens arrive far faster than a TextEdit can re-lay itself out.
    // The Deltas accumulate in m_streamText; this timer moves them into the
    // pane at most every 60 ms.
    m_streamTimer.setSingleShot(true);
    m_streamTimer.setInterval(60);
    connect(&m_streamTimer, &QTimer::timeout, this,
            &OcrResultController::refreshStreamingPane);
}

void OcrResultController::setStatusText(const QString &text, bool error)
{
    m_statusText = text;
    m_statusIsError = error;
    emit statusTextChanged();
    if (!text.isEmpty())
        m_statusTimer.start();
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
    startTranslation(-1, 0);
}

void OcrResultController::translateRange(int start, int length)
{
    startTranslation(start, length);
}

void OcrResultController::startTranslation(int start, int length)
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

    // A selection becomes the recognized lines it touches. When it is exactly
    // those whole lines, the numbered request and the picture covers still
    // apply. Anything shorter (a word, part of a line) is sent as that exact
    // text with its line as context and must not paint a cover (T4.5.2). A
    // selection that maps to no line box (a QR payload has none) is ignored,
    // which keeps translateRange() a safe "translate what I selected".
    QVector<int> selected;
    if (start >= 0 && length > 0)
        selected = selectedLineIndexes(m_lines, start, length);
    if (!selected.isEmpty() && !rangeCoversWholeLines(m_lines, start, length)) {
        if (holdForPrivacy(PendingTranslation::Range, start, length))
            return;
        startPartialTranslation(start, length);
        return;
    }
    if (holdForPrivacy(PendingTranslation::Range, start, length))
        return;
    startLineTranslation(selected, !selected.isEmpty());
}

void OcrResultController::startLineTranslation(const QVector<int> &selected,
                                               bool selectionOnly)
{
    // The numbered path: every selected line becomes one request item (split at
    // sentence boundaries when too long), and every answer lands on its box.
    m_translationPartialText = false;
    clearPartialState();
    m_translationSelectionOnly = selectionOnly;
    m_translateLines.clear();
    if (selected.isEmpty()) {
        m_translateLines = m_lines;
    } else {
        m_translateLines.reserve(selected.size());
        for (const int index : selected)
            m_translateLines.append(m_lines.at(index));
    }
    m_translateLineSource = selected;
    if (selected.isEmpty()) {
        m_translateLineSource.reserve(m_lines.size());
        for (int i = 0; i < m_lines.size(); ++i)
            m_translateLineSource.append(i);
    }
    m_translationLineCount = m_translateLines.size();
    m_sourceText = textOfLines(m_translateLines);
    // A result without line geometry (a QR payload) has nothing to map onto,
    // so the whole text is still the only sensible request.
    if (m_translateLines.isEmpty())
        m_sourceText = m_text;

    m_translationSplitLines = 0;
    m_translating = true;
    m_translationError.clear();
    m_translation.clear();
    setTranslatedLines({});
    m_translationUnchanged = false;
    emit translationChanged();

    const quint64 seq = ++m_translationSeq;
    m_chunks.clear();
    m_items.clear();
    m_itemTranslations.clear();
    m_chunkIndex = 0;
    m_chunkRetried.clear();
    m_failedLines.clear();
    m_lastChunkError.clear();
    m_wholeTextRetried = false;
    m_streamText.clear();
    m_streamTimer.stop();
    QStringList itemTexts;
    for (int i = 0; i < m_translateLines.size(); ++i) {
        const QString lineText =
            m_translateLines.at(i).toMap().value(QStringLiteral("text")).toString();
        // A line too long for one request is split at sentence boundaries, and
        // its parts are sent as separate numbered items. They are joined back
        // into the one cover they belong to once every part is translated.
        const QStringList parts =
            Translator::splitLongLine(lineText, Translator::kItemTokenBudget);
        if (parts.size() > 1)
            ++m_translationSplitLines;
        for (const QString &part : parts) {
            m_items.append(TranslationItem{i, part});
            itemTexts.append(part);
        }
    }
    if (m_translateLines.size() > 1 || m_items.size() > 1) {
        // Numbered requests, one per chunk, so every translation comes back
        // with the line box it belongs to. A model that merges or splits lines
        // gives a count mismatch and the whole-text request takes over.
        int itemIndex = 0;
        for (const QStringList &chunk :
             Translator::chunkTexts(itemTexts, Translator::kRequestTokenBudget)) {
            QVector<int> indices;
            indices.reserve(chunk.size());
            for (int i = 0; i < chunk.size(); ++i)
                indices.append(itemIndex++);
            m_chunks.append(indices);
        }
        m_itemTranslations = QVector<QString>(m_items.size());
        m_chunkRetried = QVector<bool>(m_chunks.size(), false);
        // The initial translationChanged was emitted before the chunks
        // existed; tell the window the progress count now.
        emit translationChanged();
        if (!m_chunks.isEmpty()) {
            requestNextLineChunk(seq);
            return;
        }
    }
    requestWholeText(seq);
}

bool OcrResultController::holdForPrivacy(PendingTranslation kind, int start, int length)
{
    if (!m_settings)
        return false;
    const QUrl endpoint = Translator::endpointUrl(m_settings->translationEndpoint());
    if (endpoint.host().isEmpty() || Translator::isLocalEndpoint(endpoint)
        || endpoint.host() == m_settings->translationPrivacyHost())
        return false;
    m_privacyNoticeHost = endpoint.host();
    m_privacyPending = kind;
    m_privacyStart = start;
    m_privacyLength = length;
    emit privacyNoticeChanged();
    emit privacyNoticeRequested();
    return true;
}

void OcrResultController::clearPrivacyNotice()
{
    m_privacyPending = PendingTranslation::None;
    if (m_privacyNoticeHost.isEmpty())
        return;
    m_privacyNoticeHost.clear();
    m_privacyStart = -1;
    m_privacyLength = 0;
    emit privacyNoticeChanged();
}

void OcrResultController::confirmPrivacyNotice()
{
    const QString host = m_privacyNoticeHost;
    if (host.isEmpty())
        return;
    if (m_settings)
        m_settings->setTranslationPrivacyHost(host);
    const PendingTranslation kind = m_privacyPending;
    const int start = m_privacyStart;
    const int length = m_privacyLength;
    clearPrivacyNotice();
    if (kind == PendingTranslation::SelectedLines)
        translateSelectedLines();
    else
        startTranslation(start, length);
}

void OcrResultController::cancelPrivacyNotice()
{
    clearPrivacyNotice();
}

QVariantList OcrResultController::selectedLines() const
{
    QVariantList out;
    out.reserve(m_selectedLines.size());
    for (const int index : m_selectedLines)
        out.append(index);
    return out;
}

void OcrResultController::setSelectedLines(const QVector<int> &lines)
{
    QVector<int> next;
    next.reserve(lines.size());
    for (const int index : lines) {
        if (index < 0 || index >= m_lines.size() || next.contains(index))
            continue;
        next.append(index);
    }
    std::sort(next.begin(), next.end());
    if (next == m_selectedLines)
        return;
    m_selectedLines = next;
    emit selectionChanged();
}

void OcrResultController::selectLine(int index)
{
    if (index < 0 || index >= m_lines.size())
        return;
    m_selectionAnchor = index;
    setSelectedLines({index});
}

void OcrResultController::toggleLine(int index)
{
    if (index < 0 || index >= m_lines.size())
        return;
    QVector<int> next = m_selectedLines;
    if (next.contains(index)) {
        next.removeAll(index);
        m_selectionAnchor = next.isEmpty() ? -1 : next.last();
    } else {
        next.append(index);
        m_selectionAnchor = index;
    }
    setSelectedLines(next);
}

void OcrResultController::extendLineSelection(int index)
{
    if (index < 0 || index >= m_lines.size())
        return;
    if (m_selectionAnchor < 0 || m_selectionAnchor >= m_lines.size()) {
        selectLine(index);
        return;
    }
    const int lo = std::min(m_selectionAnchor, index);
    const int hi = std::max(m_selectionAnchor, index);
    QVector<int> next;
    next.reserve(hi - lo + 1);
    for (int i = lo; i <= hi; ++i)
        next.append(i);
    setSelectedLines(next);
}

void OcrResultController::selectAllLines()
{
    if (m_lines.isEmpty())
        return;
    QVector<int> next;
    next.reserve(m_lines.size());
    for (int i = 0; i < m_lines.size(); ++i)
        next.append(i);
    m_selectionAnchor = 0;
    setSelectedLines(next);
}

void OcrResultController::clearLineSelection()
{
    m_selectionAnchor = -1;
    setSelectedLines({});
}

QString OcrResultController::selectionText() const
{
    QStringList texts;
    texts.reserve(m_selectedLines.size());
    for (const int index : m_selectedLines) {
        if (index < 0 || index >= m_lines.size())
            continue;
        texts.append(m_lines.at(index).toMap().value(QStringLiteral("text")).toString());
    }
    return texts.join(QLatin1Char('\n'));
}

void OcrResultController::translateSelectedLines()
{
    if (m_selectedLines.isEmpty()) {
        translate();
        return;
    }
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
    if (holdForPrivacy(PendingTranslation::SelectedLines, -1, 0))
        return;
    startLineTranslation(m_selectedLines, true);
}

bool OcrResultController::rangeCoversWholeLines(const QVariantList &lines, int start, int length)
{
    if (start < 0 || length <= 0 || lines.isEmpty())
        return false;
    const QVector<int> touched = selectedLineIndexes(lines, start, length);
    if (touched.isEmpty())
        return false;
    const QVariantMap first = lines.at(touched.first()).toMap();
    const QVariantMap last = lines.at(touched.last()).toMap();
    if (first.value(QStringLiteral("start")).toInt() != start)
        return false;
    return start + length
           == last.value(QStringLiteral("start")).toInt()
                  + last.value(QStringLiteral("length")).toInt();
}

bool OcrResultController::rangeCoversWholeLines(int start, int length) const
{
    return rangeCoversWholeLines(m_lines, start, length);
}

void OcrResultController::startPartialTranslation(int start, int length)
{
    m_translationSelectionOnly = false;
    m_translationPartialText = true;
    m_partialStart = start;
    m_partialLength = length;
    m_sourceText = m_text.mid(start, length);
    // The touched lines are the request's context, exactly as before.
    const QVector<int> touched = selectedLineIndexes(m_lines, start, length);
    QStringList context;
    context.reserve(touched.size());
    for (const int index : touched) {
        context.append(m_lines.at(index).toMap().value(QStringLiteral("text")).toString());
    }
    m_partialContext = context.join(QLatin1Char('\n'));
    rebuildPartialFragments();
    m_translateLines.clear();
    m_translateLineSource.clear();
    m_translationLineCount = 0;
    m_translationSplitLines = 0;
    m_translating = true;
    m_translationError.clear();
    m_translation.clear();
    setTranslatedLines({});
    m_translationUnchanged = false;
    // The answer streams into a callout from the first token: a cover would
    // resize itself on every delta, and the final decision runs at Done.
    m_partialPresentation = m_partialFragments.isEmpty() ? QStringLiteral("pane")
                                                         : QStringLiteral("callout");
    m_partialCallout = partialCalloutFor(QString());
    const quint64 seq = ++m_translationSeq;
    m_chunks.clear();
    m_items.clear();
    m_itemTranslations.clear();
    m_chunkIndex = 0;
    m_chunkRetried.clear();
    m_failedLines.clear();
    m_lastChunkError.clear();
    m_wholeTextRetried = false;
    m_streamText.clear();
    m_streamTimer.stop();
    emit translationChanged();
    requestPartialText(seq);
}

// One fragment per OCR line the selection touches: the union of the selected
// glyphs on that line, grown like a line cover and capped at the page's
// neighbouring boxes. `start`/`length` are the source range on that line, so a
// cover or a callout always stands for exactly the text that was selected.
void OcrResultController::rebuildPartialFragments()
{
    m_partialFragments.clear();
    if (m_partialStart < 0 || m_partialLength <= 0 || m_glyphs.isEmpty())
        return;
    const int selectionEnd = m_partialStart + m_partialLength;
    const QVector<int> touched = selectedLineIndexes(m_lines, m_partialStart, m_partialLength);
    for (const int lineIndex : touched) {
        const QVariantMap line = m_lines.at(lineIndex).toMap();
        const int lineStart = line.value(QStringLiteral("start")).toInt();
        const int lineLength = line.value(QStringLiteral("length")).toInt();
        const int start = std::max(m_partialStart, lineStart);
        const int end = std::min(selectionEnd, lineStart + lineLength);
        if (end <= start)
            continue;
        QRect glyphRect;
        for (const Glyph &glyph : m_glyphs) {
            if (glyph.line != lineIndex)
                continue;
            const int glyphEnd = glyph.start + glyph.length;
            if (glyph.start >= end || glyphEnd <= start)
                continue;
            glyphRect = glyphRect.isNull() ? glyph.rect : glyphRect.united(glyph.rect);
        }
        if (glyphRect.isNull())
            continue;
        QRect cover = paddedCoverRect(glyphRect, m_lineRects, lineIndex);
        // The line boxes are not enough for a word: its own line has words on
        // both sides, and the 30% sideways overshoot would otherwise reach
        // into the glyph before it. Stop at the glyphs outside this range.
        capCoverAtGlyphs(m_glyphs, start, end - start, glyphRect, &cover);
        QVariantMap fragment;
        fragment.insert(QStringLiteral("start"), start);
        fragment.insert(QStringLiteral("length"), end - start);
        fragment.insert(QStringLiteral("text"), m_text.mid(start, end - start));
        fragment.insert(QStringLiteral("x"), cover.x());
        fragment.insert(QStringLiteral("y"), cover.y());
        fragment.insert(QStringLiteral("width"), cover.width());
        fragment.insert(QStringLiteral("height"), cover.height());
        fragment.insert(QStringLiteral("line"), lineIndex);
        m_partialFragments.append(fragment);
    }
}

QVariantMap OcrResultController::partialCalloutFor(const QString &translation) const
{
    if (m_partialFragments.isEmpty())
        return {};
    const QVariantMap first = m_partialFragments.first().toMap();
    QVariantMap callout;
    callout.insert(QStringLiteral("source"), m_text.mid(m_partialStart, m_partialLength));
    callout.insert(QStringLiteral("translation"), translation);
    callout.insert(QStringLiteral("x"), first.value(QStringLiteral("x")));
    callout.insert(QStringLiteral("y"), first.value(QStringLiteral("y")));
    callout.insert(QStringLiteral("width"), first.value(QStringLiteral("width")));
    callout.insert(QStringLiteral("height"), first.value(QStringLiteral("height")));
    return callout;
}

// The decision, in one place so the streaming path and the preview seam cannot
// disagree: no fragment means the pane; more than one means one callout; one
// fragment is a cover only when the fitted size clears the readability floor,
// otherwise a callout anchored to it.
void OcrResultController::applyPartialAnswer(const QString &answer)
{
    setTranslatedLines({});
    m_partialCallout.clear();
    // Reset first: a previous answer may have left "cover" behind, and the
    // one-fragment branch below only ever sets "cover" itself.
    m_partialPresentation = QStringLiteral("pane");
    m_translation = answer;
    m_translationError.clear();
    m_translationUnchanged = translationLooksUnchanged(m_sourceText, answer);
    if (m_partialFragments.isEmpty()) {
        m_partialPresentation = QStringLiteral("pane");
    } else if (m_partialFragments.size() > 1 || answer.isEmpty()) {
        // Covers are single-line only: splitting a model answer across lines is
        // guesswork, so a selection crossing a line boundary gets one callout.
        m_partialPresentation = QStringLiteral("callout");
        m_partialCallout = partialCalloutFor(answer);
    } else {
        QString joined;
        QVariantList map = translatedLineMap(m_partialFragments, {answer}, m_image, &joined,
                                             false);
        if (!map.isEmpty()) {
            QVariantMap cover = map.first().toMap();
            const int fitted = cover.value(QStringLiteral("fontSize")).toInt();
            if (fitted >= kMinPartialCoverPixelSize
                && !cover.value(QStringLiteral("overflow")).toBool()) {
                // Screen only: Save image and Copy image keep the page.
                cover.insert(QStringLiteral("screenOnly"), true);
                map[0] = cover;
                setTranslatedLines(map);
                m_partialPresentation = QStringLiteral("cover");
                m_translation = joined;
            }
        }
        if (m_partialPresentation != QStringLiteral("cover")) {
            m_partialPresentation = QStringLiteral("callout");
            m_partialCallout = partialCalloutFor(answer);
        }
    }
    emit translationChanged();
    if (!answer.isEmpty())
        emit translationReady();
}

void OcrResultController::clearPartialState()
{
    m_partialStart = -1;
    m_partialLength = 0;
    m_partialFragments.clear();
    m_partialPresentation = QStringLiteral("pane");
    m_partialCallout.clear();
    m_partialContext.clear();
}

void OcrResultController::setPartialTranslationForPreview(int start, int length,
                                                         const QString &answer)
{
    if (m_text.isEmpty() || start < 0 || length <= 0 || start + length > m_text.size())
        return;
    clearPrivacyNotice();
    ++m_translationSeq;
    if (m_translator)
        m_translator->cancel();
    m_translating = false;
    m_streamTimer.stop();
    m_streamText.clear();
    m_items.clear();
    m_itemTranslations.clear();
    m_chunks.clear();
    m_chunkIndex = 0;
    m_chunkRetried.clear();
    m_failedLines.clear();
    m_lastChunkError.clear();
    m_wholeTextRetried = false;
    m_translationSelectionOnly = false;
    m_translationPartialText = true;
    m_partialStart = start;
    m_partialLength = length;
    m_sourceText = m_text.mid(start, length);
    const QVector<int> touched = selectedLineIndexes(m_lines, start, length);
    QStringList context;
    for (const int index : touched)
        context.append(m_lines.at(index).toMap().value(QStringLiteral("text")).toString());
    m_partialContext = context.join(QLatin1Char('\n'));
    rebuildPartialFragments();
    m_translateLines.clear();
    m_translateLineSource.clear();
    m_translationLineCount = 0;
    m_translationSplitLines = 0;
    applyPartialAnswer(answer);
}

void OcrResultController::requestPartialText(quint64 seq)
{
    if (seq != m_translationSeq)
        return;
    m_streamText.clear();
    m_streamTimer.stop();
    m_translator->translateStreaming(
        Translator::partialPrompt(translationTarget()),
        Translator::partialRequest(m_partialContext, m_sourceText),
        [this, seq](const QString &partial) {
            if (seq != m_translationSeq)
                return;
            m_streamText += partial;
            if (!m_streamTimer.isActive())
                m_streamTimer.start();
        },
        [this, seq](const QString &text, const QString &error, bool retryable) {
            if (seq != m_translationSeq)
                return;
            m_streamText.clear();
            m_streamTimer.stop();
            if (!error.isEmpty()) {
                // The same single retry every other request path gets.
                if (retryable && !m_wholeTextRetried) {
                    m_wholeTextRetried = true;
                    QTimer::singleShot(kChunkRetryDelayMs, this, [this, seq] {
                        if (seq == m_translationSeq)
                            requestPartialText(seq);
                    });
                    return;
                }
                m_translating = false;
                m_translationError = error;
                m_translation.clear();
                setTranslatedLines({});
                clearPartialState();
                emit translationChanged();
                return;
            }
            m_translating = false;
            applyPartialAnswer(text);
        });
}


void OcrResultController::requestNextLineChunk(quint64 seq)
{
    if (seq != m_translationSeq)
        return;
    if (m_chunkIndex >= m_chunks.size()) {
        finishLineTranslation();
        return;
    }
    const QVector<int> chunk = m_chunks.at(m_chunkIndex);
    QStringList texts;
    texts.reserve(chunk.size());
    for (const int index : chunk)
        texts.append(m_items.at(index).text);
    m_streamText.clear();
    m_streamTimer.stop();
    m_translator->translateStreaming(
        Translator::numberedPrompt(translationTarget()), Translator::numberedRequest(texts),
        [this, seq](const QString &partial) {
            if (seq != m_translationSeq)
                return;
            m_streamText += partial;
            if (!m_streamTimer.isActive())
                m_streamTimer.start();
        },
        [this, seq, chunk, texts](const QString &body, const QString &error, bool retryable) {
            if (seq != m_translationSeq)
                return;
            m_streamText.clear();
            m_streamTimer.stop();
            if (!error.isEmpty()) {
                // One retry with a short backoff on a transport error or 5xx.
                // A 4xx will fail again, so that chunk is recorded as failed.
                if (retryable && !m_chunkRetried.value(m_chunkIndex, true)) {
                    m_chunkRetried[m_chunkIndex] = true;
                    QTimer::singleShot(kChunkRetryDelayMs, this, [this, seq] {
                        if (seq == m_translationSeq)
                            requestNextLineChunk(seq);
                    });
                    return;
                }
                m_lastChunkError = error;
                for (const int index : chunk) {
                    const int line = m_items.at(index).line;
                    const int sourceLine = line >= 0 && line < m_translateLineSource.size()
                                               ? m_translateLineSource.at(line)
                                               : line;
                    if (!m_failedLines.contains(sourceLine))
                        m_failedLines.append(sourceLine);
                }
                std::sort(m_failedLines.begin(), m_failedLines.end());
                ++m_chunkIndex;
                refreshStreamingPane();
                emit translationChanged();
                requestNextLineChunk(seq);
                return;
            }
            QString parseError;
            const QStringList parsed =
                Translator::parseNumberedLines(body, texts.size(), &parseError);
            if (parsed.size() != texts.size()) {
                // The service answered, but not line by line: the translation is
                // still worth showing, just in the pane instead of on the picture.
                m_items.clear();
                m_itemTranslations.clear();
                m_chunks.clear();
                m_chunkIndex = 0;
                m_failedLines.clear();
                requestWholeText(seq);
                return;
            }
            for (int i = 0; i < chunk.size() && i < parsed.size(); ++i)
                m_itemTranslations[chunk.at(i)] = parsed.at(i);
            ++m_chunkIndex;
            refreshStreamingPane();
            emit translationChanged();
            requestNextLineChunk(seq);
        });
}

void OcrResultController::finishLineTranslation()
{
    const QStringList lineTranslations = assembledLineTranslations();
    QStringList translated;
    QVariantList mapLines;
    for (int i = 0; i < lineTranslations.size(); ++i) {
        if (lineTranslations.at(i).isEmpty())
            continue; // a chunk that did not translate has no cover
        mapLines.append(m_translateLines.at(i));
        translated.append(lineTranslations.at(i));
    }
    QString text;
    setTranslatedLines(mapLines.isEmpty()
                           ? QVariantList{}
                           : translatedLineMap(mapLines, translated, m_image, &text, true,
                                               m_glyphs));
    m_translation = text;
    if (!m_failedLines.isEmpty())
        m_translationError = failedLinesSummary();
    else if (m_translation.isEmpty())
        m_translationError = tr("The translation service returned an empty result");
    else
        m_translationError.clear();
    m_translationUnchanged = translationLooksUnchanged(m_sourceText, m_translation);
    m_translating = false;
    m_streamText.clear();
    m_streamTimer.stop();
    emit translationChanged();
    if (!m_translation.isEmpty())
        emit translationReady();
}

QString OcrResultController::failedLinesSummary() const
{
    // Contiguous 0-based line indices become "3", "3-5" or "3-5, 8"; the
    // window shows 1-based numbers because that is what a reader counts.
    QStringList ranges;
    int start = -1;
    int previous = -1;
    const auto flush = [&ranges, &start, &previous] {
        if (start < 0)
            return;
        ranges.append(start == previous
                          ? QString::number(start + 1)
                          : QStringLiteral("%1-%2").arg(start + 1).arg(previous + 1));
    };
    for (const int line : m_failedLines) {
        if (start < 0) {
            start = previous = line;
            continue;
        }
        if (line == previous + 1) {
            previous = line;
            continue;
        }
        flush();
        start = previous = line;
    }
    flush();
    const QString which = ranges.join(QStringLiteral(", "));
    const QString detail = m_lastChunkError.isEmpty()
                               ? tr("the translation service did not answer")
                               : m_lastChunkError;
    return m_translationSelectionOnly
               ? tr("Selected lines %1 did not translate: %2").arg(which, detail)
               : tr("Lines %1 did not translate: %2").arg(which, detail);
}

void OcrResultController::requestWholeText(quint64 seq)
{
    if (seq != m_translationSeq)
        return;
    m_streamText.clear();
    m_streamTimer.stop();
    m_translator->translateStreaming(
        Translator::systemPrompt(translationTarget()), m_sourceText,
        [this, seq](const QString &partial) {
            if (seq != m_translationSeq)
                return;
            m_streamText += partial;
            if (!m_streamTimer.isActive())
                m_streamTimer.start();
        },
        [this, seq](const QString &text, const QString &error, bool retryable) {
            if (seq != m_translationSeq)
                return;
            m_streamText.clear();
            m_streamTimer.stop();
            if (!error.isEmpty()) {
                // The whole-text request is the last resort, so it gets the
                // same single retry a chunk gets before the window gives up.
                if (retryable && !m_wholeTextRetried) {
                    m_wholeTextRetried = true;
                    QTimer::singleShot(kChunkRetryDelayMs, this, [this, seq] {
                        if (seq == m_translationSeq)
                            requestWholeText(seq);
                    });
                    return;
                }
                m_translating = false;
                m_translationError = error;
                emit translationChanged();
                return;
            }
            m_translating = false;
            m_translation = text;
            m_translationError.clear();
            setTranslatedLines({});
            // The whole-text answer may still be one line per input line (the
            // service ignored the numbering but translated line by line): map it
            // back onto the picture with the same tolerant parser instead of
            // leaving the covers empty.
            if (m_translateLines.size() > 1) {
                const QStringList rows =
                    Translator::parseNumberedLines(text, int(m_translateLines.size()), nullptr);
                if (rows.size() == m_translateLines.size()) {
                    QString joined;
                    const QVariantList map =
                        translatedLineMap(m_translateLines, rows, m_image, &joined, true,
                                          m_glyphs);
                    m_translation = joined;
                    setTranslatedLines(map);
                }
            }
            m_translationUnchanged = translationLooksUnchanged(m_sourceText, m_translation);
            if (m_translateLines.size() == 1) {
                // One line box: the whole translation is that line's translation,
                // so the overlay works without a numbered round trip.
                QString joined;
                const QVariantList map =
                    translatedLineMap(m_translateLines, QStringList{text}, m_image, &joined,
                                      true, m_glyphs);
                if (joined == text)
                    setTranslatedLines(map);
            }
            emit translationChanged();
            if (!m_translation.isEmpty())
                emit translationReady();
        });
}

QStringList OcrResultController::assembledLineTranslations() const
{
    QStringList out;
    out.reserve(m_translateLines.size());
    int next = 0;
    for (int line = 0; line < m_translateLines.size(); ++line) {
        QStringList parts;
        while (next < m_items.size() && m_items.at(next).line == line) {
            if (next < m_itemTranslations.size() && !m_itemTranslations.at(next).isEmpty())
                parts.append(m_itemTranslations.at(next));
            ++next;
        }
        out.append(parts.join(QLatin1Char(' ')));
    }
    return out;
}

QString OcrResultController::streamingPaneText() const
{
    if (m_items.isEmpty())
        return m_streamText.trimmed();
    QStringList itemTexts;
    itemTexts.reserve(m_itemTranslations.size());
    for (const QString &value : m_itemTranslations)
        itemTexts.append(value);
    // The chunk in flight has no committed answer yet, but the parser can map
    // the part that has already arrived, so the pane fills line by line.
    if (m_chunkIndex < m_chunks.size() && !m_streamText.isEmpty()) {
        const QVector<int> &chunk = m_chunks.at(m_chunkIndex);
        const QStringList partial =
            Translator::parseNumberedLinesPartial(m_streamText, chunk.size());
        for (int i = 0; i < partial.size() && i < chunk.size(); ++i)
            itemTexts[chunk.at(i)] = partial.at(i);
    }
    QStringList lines;
    int next = 0;
    for (int line = 0; line < m_translateLines.size(); ++line) {
        QStringList parts;
        while (next < m_items.size() && m_items.at(next).line == line) {
            if (next < itemTexts.size() && !itemTexts.at(next).isEmpty())
                parts.append(itemTexts.at(next));
            ++next;
        }
        if (!parts.isEmpty())
            lines.append(parts.join(QLatin1Char(' ')));
    }
    return lines.join(QLatin1Char('\n'));
}

void OcrResultController::refreshStreamingPane()
{
    if (!m_translating)
        return;
    const QString text = streamingPaneText();
    if (text == m_translation)
        return;
    m_translation = text;
    // A partial answer shows in its callout while the request is still open;
    // the cover/callout decision itself runs once, at Done.
    if (m_translationPartialText && m_partialPresentation == QLatin1String("callout"))
        m_partialCallout.insert(QStringLiteral("translation"), text);
    emit translationChanged();
}

void OcrResultController::failTranslation(quint64 seq, const QString &error)
{
    if (seq != m_translationSeq)
        return;
    m_translating = false;
    m_streamText.clear();
    m_streamTimer.stop();
    m_translation.clear();
    setTranslatedLines({});
    clearPartialState();
    m_translationError = error;
    emit translationChanged();
}

void OcrResultController::cancelTranslation()
{
    // A held first-send notice belongs to a request that never started: Cancel
    // and closing the window both drop it.
    clearPrivacyNotice();
    // Bump the generation first: a reply that lands after this must not touch
    // the new state.
    ++m_translationSeq;
    if (m_translator)
        m_translator->cancel();
    m_streamTimer.stop();
    if (m_translating) {
        m_translating = false;
        m_streamText.clear();
        m_translationPartialText = false;
        clearPartialState();
        // A partial answer is not a translation: drop it instead of leaving
        // half a page on screen after Cancel.
        m_translation.clear();
        setTranslatedLines({});
        emit translationChanged();
    }
}

void OcrResultController::setTranslationForPreview(const QStringList &translated)
{
    if (m_lines.isEmpty() || translated.isEmpty())
        return;
    clearPrivacyNotice();
    ++m_translationSeq;
    if (m_translator)
        m_translator->cancel();
    m_translating = false;
    m_translationError.clear();
    m_translationPartialText = false;
    clearPartialState();
    m_streamTimer.stop();
    m_streamText.clear();
    m_items.clear();
    m_itemTranslations.clear();
    m_chunks.clear();
    m_chunkIndex = 0;
    m_chunkRetried.clear();
    m_failedLines.clear();
    m_lastChunkError.clear();
    m_wholeTextRetried = false;
    m_translateLines = m_lines;
    m_translateLineSource.clear();
    for (int i = 0; i < m_lines.size(); ++i)
        m_translateLineSource.append(i);
    m_sourceText = m_text;
    m_translationSelectionOnly = false;
    m_translationLineCount = m_translateLines.size();
    m_translationSplitLines = 0;
    QString joined;
    setTranslatedLines(
        translatedLineMap(m_translateLines, translated, m_image, &joined, true, m_glyphs));
    m_translation = joined;
    m_translationUnchanged = translationLooksUnchanged(m_sourceText, m_translation);
    emit translationChanged();
    if (!m_translation.isEmpty())
        emit translationReady();
}

void OcrResultController::setResult(const QImage &image, const QVector<OcrWord> &words,
                                    const QString &text)
{
    ++m_translationSeq;
    if (m_translator)
        m_translator->cancel();
    m_translation.clear();
    setTranslatedLines({});
    m_translationUnchanged = false;
    m_translateLines.clear();
    m_translateLineSource.clear();
    m_sourceText.clear();
    m_translationSelectionOnly = false;
    m_translationLineCount = 0;
    m_translationSplitLines = 0;
    m_chunks.clear();
    m_items.clear();
    m_itemTranslations.clear();
    m_chunkIndex = 0;
    m_chunkRetried.clear();
    m_failedLines.clear();
    m_lastChunkError.clear();
    m_wholeTextRetried = false;
    m_streamText.clear();
    m_streamTimer.stop();
    m_translationError.clear();
    m_translating = false;
    m_translationPartialText = false;
    clearPartialState();
    clearPrivacyNotice();
    m_selectedLines.clear();
    m_selectionAnchor = -1;
    emit selectionChanged();
    m_image = image;
    m_lines.clear();
    m_lineRects.clear();
    m_glyphs.clear();
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
        // A glyph's range is relative to its line until flush() knows where
        // that line starts in `built`.
        QVector<Glyph> lineGlyphs;
        const auto flush = [&] {
            if (currentLine < 0 || lineText.isEmpty()) {
                lineGlyphs.clear();
                return;
            }
            if (!built.isEmpty())
                built += QLatin1Char('\n');
            const int start = int(built.size());
            built += lineText;
            const int lineIndex = int(m_lines.size());
            QVariantMap line;
            line.insert(QStringLiteral("text"), lineText);
            line.insert(QStringLiteral("x"), lineRect.x());
            line.insert(QStringLiteral("y"), lineRect.y());
            line.insert(QStringLiteral("width"), lineRect.width());
            line.insert(QStringLiteral("height"), lineRect.height());
            line.insert(QStringLiteral("start"), start);
            line.insert(QStringLiteral("length"), lineText.size());
            m_lines.append(line);
            m_lineRects.append(lineRect);
            for (Glyph glyph : lineGlyphs) {
                glyph.start += start;
                glyph.line = lineIndex;
                m_glyphs.append(glyph);
            }
            lineGlyphs.clear();
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
            Glyph glyph;
            glyph.start = int(lineText.size());
            glyph.length = int(w.text.size());
            glyph.rect = w.rect;
            glyph.spaceBefore = w.spaceBefore;
            lineGlyphs.append(glyph);
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
    // Both caps matter: a page of 40 very long lines still has to fit a small
    // model's context, and a page of 400 short ones still needs a bounded
    // number of requests.
    return Translator::chunkTexts(lines, Translator::kRequestTokenBudget, perRequest);
}

QVariantList OcrResultController::translatedLineMap(const QVariantList &lines,
                                                    const QStringList &translated,
                                                    const QImage &image, QString *text,
                                                    bool padCover,
                                                    const QVector<Glyph> &glyphs)
{
    // Convert once: sampleLineColors() would otherwise deep-copy a 4K capture
    // for every line box.
    QImage source = image;
    if (!source.isNull() && source.format() != QImage::Format_ARGB32
        && source.format() != QImage::Format_RGB32)
        source = source.convertToFormat(QImage::Format_ARGB32);
    QVariantList map;
    QString joined;
    // Convert the input boxes once: the neighbour scan below is per line and
    // would otherwise convert the whole list of maps for every cover.
    QVector<QRect> boxes;
    QVector<int> sourceStarts;
    QVector<int> sourceLengths;
    boxes.reserve(lines.size());
    sourceStarts.reserve(lines.size());
    sourceLengths.reserve(lines.size());
    for (const QVariant &value : lines) {
        const QVariantMap line = value.toMap();
        boxes.append(QRect(line.value(QStringLiteral("x")).toInt(),
                           line.value(QStringLiteral("y")).toInt(),
                           line.value(QStringLiteral("width")).toInt(),
                           line.value(QStringLiteral("height")).toInt()));
        sourceStarts.append(line.value(QStringLiteral("start")).toInt());
        sourceLengths.append(line.value(QStringLiteral("length")).toInt());
    }
    for (int i = 0; i < boxes.size(); ++i) {
        const QRect rect = boxes.at(i);
        const QString value = i < translated.size() ? translated.at(i) : QString();
        if (i)
            joined += QLatin1Char('\n');
        // A partial fragment already carries its padded rect (it had to cap at
        // the page's neighbours), so it must not be grown a second time. A
        // whole line is capped again at the glyphs just outside it, so a label
        // in another box on the same row is not painted over either.
        QRect cover = padCover ? paddedCoverRect(rect, boxes, i) : rect;
        if (padCover && !glyphs.isEmpty())
            capCoverAtGlyphs(glyphs, sourceStarts.at(i), sourceLengths.at(i), rect, &cover);
        const LineColors colors = sampleLineColors(source, cover);
        // The ONE font decision for this cover, in image pixels: the window
        // scales it and the composite draws at it, so the saved picture cannot
        // pick a different size than the screen shows. A CJK answer is set a
        // little below the largest size that fits, because those glyphs fill
        // the em box and otherwise read larger than the page they replace.
        bool fits = true;
        int fontSize = fittedPixelSize(value, cover.size(), std::max(8, cover.height()),
                                       &fits);
        if (fits && hasCjkGlyphs(value))
            fontSize = std::max(kMinFitPixelSize,
                                int(std::lround(fontSize * kCjkCoverSizeScale)));
        QVariantMap entry;
        entry.insert(QStringLiteral("text"), value);
        entry.insert(QStringLiteral("start"), int(joined.size()));
        entry.insert(QStringLiteral("length"), value.size());
        // The range in the SOURCE text this cover came from, so a selection
        // made on the Translation layer can be mapped back to the source
        // lines and re-translated without guessing.
        entry.insert(QStringLiteral("sourceStart"), sourceStarts.at(i));
        entry.insert(QStringLiteral("sourceLength"), sourceLengths.at(i));
        entry.insert(QStringLiteral("x"), cover.x());
        entry.insert(QStringLiteral("y"), cover.y());
        entry.insert(QStringLiteral("width"), cover.width());
        entry.insert(QStringLiteral("height"), cover.height());
        entry.insert(QStringLiteral("background"), colors.background.name(QColor::HexRgb));
        entry.insert(QStringLiteral("foreground"), colors.foreground.name(QColor::HexRgb));
        entry.insert(QStringLiteral("fontSize"), fontSize);
        entry.insert(QStringLiteral("rtl"), lineIsRtl(value));
        entry.insert(QStringLiteral("overflow"), !fits);
        map.append(entry);
        joined += value;
    }
    if (text)
        *text = joined;
    return map;
}

// The fitting rule, in one place. `Text.Fit` used to run this inside QML and
// the saved picture would have had to run it a second time: the screen and the
// file could then disagree on the size for the same cover.
int OcrResultController::fittedPixelSize(const QString &text, const QSize &box, int maxPixelSize,
                                         bool *fits)
{
    const int maxSize = std::max(kMinFitPixelSize, maxPixelSize);
    if (text.isEmpty()) {
        if (fits)
            *fits = true;
        return maxSize;
    }
    if (box.width() <= 0 || box.height() <= 0) {
        if (fits)
            *fits = false;
        return kMinFitPixelSize;
    }
    // Downwards, not a binary search: a larger size can, in principle, break a
    // line differently and come back a row shorter, and the first size that
    // fits is the one QML's Text.Fit would have chosen too.
    QFont font = QGuiApplication::font();
    for (int size = maxSize; size >= kMinFitPixelSize; --size) {
        font.setPixelSize(size);
        QTextLayout layout(text, font);
        layOutText(layout, box.width(), false);
        if (layout.lineCount() > 0 && laidOutHeight(layout) <= box.height()) {
            if (fits)
                *fits = true;
            return size;
        }
    }
    if (fits)
        *fits = false;
    return kMinFitPixelSize;
}

bool OcrResultController::lineIsRtl(const QString &text)
{
    for (const QChar c : text) {
        switch (c.direction()) {
        case QChar::DirR:
        case QChar::DirAL:
        case QChar::DirRLE:
        case QChar::DirRLO:
            return true;
        case QChar::DirL:
        case QChar::DirLRE:
        case QChar::DirLRO:
            return false;
        default:
            // Digits, spaces and punctuation say nothing about the paragraph
            // direction; the first strong character decides, as in Unicode.
            break;
        }
    }
    return false;
}

int OcrResultController::lineIndexAt(const QVariantList &lines, const QPointF &imagePoint)
{
    // Backwards: the covers paint in list order, so the last one drawn is the
    // one the eye (and the pointer) sees where two padded boxes overlap.
    const QPoint point(int(std::floor(imagePoint.x())), int(std::floor(imagePoint.y())));
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QVariantMap line = lines.at(i).toMap();
        const QRect rect(line.value(QStringLiteral("x")).toInt(),
                         line.value(QStringLiteral("y")).toInt(),
                         line.value(QStringLiteral("width")).toInt(),
                         line.value(QStringLiteral("height")).toInt());
        if (rect.contains(point))
            return i;
    }
    return -1;
}

QVector<int> OcrResultController::selectedLineIndexes(const QVariantList &lines, int start,
                                                      int length)
{
    QVector<int> selected;
    if (start < 0 || length <= 0)
        return selected;
    const int end = start + length;
    selected.reserve(8);
    for (int i = 0; i < lines.size(); ++i) {
        const QVariantMap line = lines.at(i).toMap();
        const int lineStart = line.value(QStringLiteral("start")).toInt();
        const int lineLength = line.value(QStringLiteral("length")).toInt();
        if (start < lineStart + lineLength && end > lineStart)
            selected.append(i);
    }
    return selected;
}

QImage OcrResultController::composeOverlay(const QImage &image, const QVariantList &translatedLines)
{
    if (image.isNull() || translatedLines.isEmpty())
        return image;
    // A word cover is screen only: it must never be baked into a saved or
    // copied picture. If every entry is screen only, the page is untouched.
    bool paints = false;
    for (const QVariant &value : translatedLines) {
        if (!value.toMap().value(QStringLiteral("screenOnly")).toBool()) {
            paints = true;
            break;
        }
    }
    if (!paints)
        return image;
    QImage out = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QFont baseFont = QGuiApplication::font();

    for (const QVariant &value : translatedLines) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("screenOnly")).toBool())
            continue;
        const QRect cover(entry.value(QStringLiteral("x")).toInt(),
                          entry.value(QStringLiteral("y")).toInt(),
                          entry.value(QStringLiteral("width")).toInt(),
                          entry.value(QStringLiteral("height")).toInt());
        if (cover.isEmpty())
            continue;
        const QColor background(entry.value(QStringLiteral("background")).toString());
        const QColor foreground(entry.value(QStringLiteral("foreground")).toString());
        const bool rtl = entry.value(QStringLiteral("rtl")).toBool();
        const QString text = entry.value(QStringLiteral("text")).toString();
        const int radius = std::min(3, cover.height() / 4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(background.isValid() ? background : QColor(255, 255, 255));
        painter.drawRoundedRect(cover, radius, radius);
        if (text.isEmpty())
            continue;

        QFont font = baseFont;
        font.setPixelSize(std::max(kMinFitPixelSize, entry.value(QStringLiteral("fontSize")).toInt()));
        QTextLayout layout(text, font);
        layOutText(layout, cover.width(), rtl);
        const QFontMetrics metrics(font);
        const int total = laidOutHeight(layout);
        const int top = cover.y() + (cover.height() - total) / 2;
        for (int i = 0; i < layout.lineCount(); ++i) {
            QTextLine line = layout.lineAt(i);
            line.setPosition(QPointF(0, i * metrics.lineSpacing()));
        }
        painter.save();
        painter.setClipRect(cover);
        // QTextLayout::draw takes the text colour from the pen, and the cover
        // fill above left it at Qt::NoPen: without this the saved picture has
        // empty covers while the screen shows text.
        painter.setPen(foreground.isValid() ? foreground : QColor(0, 0, 0));
        layout.draw(&painter, QPointF(cover.x(), top));
        painter.restore();

        // A translation that cannot fit even at the 5 px floor is clipped by
        // the cover anyway; mark the corner instead of pretending it fits.
        if (entry.value(QStringLiteral("overflow")).toBool()) {
            const int dot = std::clamp(cover.height() / 5, 3, 8);
            QColor marker = foreground.isValid() ? foreground : QColor(0, 0, 0);
            marker.setAlpha(200);
            painter.setPen(Qt::NoPen);
            painter.setBrush(marker);
            painter.drawRoundedRect(QRect(cover.right() - dot - 1, cover.bottom() - dot - 1, dot, dot),
                                    1, 1);
        }
    }
    painter.end();
    return out;
}

QImage OcrResultController::compositedImage(bool translated) const
{
    if (m_image.isNull())
        return {};
    if (!translated || m_translatedLines.isEmpty())
        return m_image;
    return composeOverlay(m_image, m_translatedLines);
}

int OcrResultController::lineAt(const QPointF &imagePoint, bool translated) const
{
    const QVector<QRect> &rects = translated ? m_coverRects : m_lineRects;
    const QPoint point(int(std::floor(imagePoint.x())), int(std::floor(imagePoint.y())));
    for (int i = rects.size() - 1; i >= 0; --i) {
        if (rects.at(i).contains(point))
            return i;
    }
    return -1;
}

void OcrResultController::setTranslatedLines(const QVariantList &lines)
{
    m_translatedLines = lines;
    m_coverRects.clear();
    m_coverRects.reserve(lines.size());
    for (const QVariant &value : lines) {
        const QVariantMap entry = value.toMap();
        m_coverRects.append(QRect(entry.value(QStringLiteral("x")).toInt(),
                                  entry.value(QStringLiteral("y")).toInt(),
                                  entry.value(QStringLiteral("width")).toInt(),
                                  entry.value(QStringLiteral("height")).toInt()));
    }
}
