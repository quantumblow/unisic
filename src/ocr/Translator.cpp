#include "Translator.h"

#include "Settings.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

#include <cmath>

Translator::Translator(Settings *settings, QObject *parent) : QObject(parent), m_settings(settings) {}

Translator::~Translator() = default;

QUrl Translator::endpointUrl(const QString &base)
{
    QString value = base.trimmed();
    if (value.isEmpty())
        return {};
    while (value.endsWith(QLatin1Char('/')))
        value.chop(1);
    if (value.endsWith(QLatin1String("/chat/completions")))
        return QUrl(value);
    if (value.endsWith(QLatin1String("/v1")))
        return QUrl(value + QStringLiteral("/chat/completions"));
    return QUrl(value + QStringLiteral("/v1/chat/completions"));
}

bool Translator::isLocalEndpoint(const QUrl &url)
{
    const QString host = url.host().toLower();
    return host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1")
           || host == QLatin1String("::1");
}

QString Translator::systemPrompt(const QString &target)
{
    return QStringLiteral(
               "You are a translation engine. Translate the user's text into %1. "
               "The text may be in another language; translate it and never return it unchanged. "
               "Keep the original line breaks, numbers, punctuation and order. "
               "Return only the translation, with no explanation, notes or quotation marks.")
        .arg(target);
}

QString Translator::numberedPrompt(const QString &target)
{
    return QStringLiteral(
               "You are a translation engine. The user sends numbered lines of text taken from a "
               "screenshot. Translate every line into %1. Answer with the same number of lines and the "
               "same numbering, one numbered line per input line, never merged and never split, even "
               "when an input line is a single word or a number. The text may be in another language; "
               "translate it and never return an input line unchanged. Start the answer with \"1.\" and "
               "keep the lines in order. Return only the numbered translation, with no explanation, "
               "notes, quotation marks, markdown or code fences.")
        .arg(target);
}

QString Translator::partialPrompt(const QString &target)
{
    return QStringLiteral(
               "You are a translation engine. The user selected part of a line from a screenshot. "
               "Translate into %1 only the text under \"Selected text\". The whole line is given "
               "under \"Context\" so the fragment's meaning is clear; use it, but do not translate "
               "or repeat the context, and do not add words that are not in the selected text. "
               "Keep the selected text's own punctuation. Return only the translation of the "
               "selected text, with no explanation, notes, quotation marks or quotation of the "
               "context.")
        .arg(target);
}

QString Translator::partialRequest(const QString &context, const QString &selection)
{
    return QStringLiteral("Context: %1\nSelected text: %2").arg(context, selection);
}

QString Translator::numberedRequest(const QStringList &lines)
{
    QStringList numbered;
    numbered.reserve(lines.size());
    for (int i = 0; i < lines.size(); ++i)
        numbered.append(QStringLiteral("%1. %2").arg(i + 1).arg(lines.at(i)));
    return numbered.join(QLatin1Char('\n'));
}

namespace {

QString stripMarks(QString value)
{
    value = value.trimmed();
    while (value.startsWith(QLatin1String("**")))
        value = value.mid(2).trimmed();
    while (value.endsWith(QLatin1String("**")) && value.size() > 2)
        value = value.left(value.size() - 2).trimmed();
    return value;
}

// Answer lines in order, without blanks, code fences or leading bullets.
QStringList answerRows(const QString &text)
{
    QStringList rows;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        QString row = raw.trimmed();
        if (row.isEmpty() || row.startsWith(QLatin1String("```")))
            continue;
        int start = 0;
        while (start < row.size()
               && (row.at(start) == QLatin1Char('*') || row.at(start) == QLatin1Char('#')
                   || row.at(start) == QLatin1Char('>') || row.at(start) == QChar(0x2022)))
            ++start;
        row = row.mid(start).trimmed();
        if (!row.isEmpty())
            rows.append(row);
    }
    return rows;
}

// The number a row starts with, or -1 when it does not start with one. "1.",
// "1)", "[1]", "**1.**" and a bare "1 text" are all accepted; anything else
// means the model rewrote the layout.
int rowNumber(const QString &row, QString *rest)
{
    static const QRegularExpression marker(
        QStringLiteral("^(\\d{1,3})\\s*[*]*\\s*[.)\\]\\-:：、]?\\s*(.*)$"));
    const int skip = row.startsWith(QLatin1Char('[')) ? 1 : 0;
    const QRegularExpressionMatch match = marker.match(row.mid(skip));
    if (!match.hasMatch())
        return -1;
    if (rest)
        *rest = match.captured(2);
    return match.captured(1).toInt();
}

} // namespace

QStringList Translator::parseNumberedLines(const QString &text, int expected, QString *error)
{
    if (error)
        error->clear();
    const auto fail = [error] {
        if (error)
            *error = tr("The translation service did not keep the line numbering");
        return QStringList{};
    };
    if (expected <= 0)
        return fail();
    const QStringList rows = answerRows(text);
    if (rows.isEmpty())
        return fail();

    // Numbered block: start at the line that carries "1" (a sentence before it
    // is a preamble, not a translation), then walk the sequence. An unnumbered
    // line inside the block is a wrapped continuation of the line before it; a
    // sentence after the last expected line is noise.
    int first = -1;
    for (int i = 0; i < rows.size(); ++i) {
        if (rowNumber(rows.at(i), nullptr) == 1) {
            first = i;
            break;
        }
    }
    if (first >= 0) {
        QStringList out;
        for (int i = first; i < rows.size(); ++i) {
            QString rest;
            const int number = rowNumber(rows.at(i), &rest);
            if (number == out.size() + 1) {
                out.append(stripMarks(rest));
                continue;
            }
            if (out.size() >= expected)
                break; // a closing sentence after the last translation line
            if (number < 0) {
                out.last() += QLatin1Char(' ') + stripMarks(rows.at(i));
                continue;
            }
            // A numbered line out of sequence: the model rewrote the layout.
            out.clear();
            break;
        }
        if (out.size() == expected)
            return out;
    }

    // Positional fallback: exactly one answer line per input line. A model that
    // translated line by line but dropped the numbering is still usable; when a
    // numbering block was found first, strip its numbers so the picture never
    // shows "1." in front of a translation.
    if (rows.size() == expected) {
        QStringList plain;
        for (const QString &row : rows) {
            QString rest;
            const int number = rowNumber(row, &rest);
            // Strip a number that could be a line number (1-based, 0-based, or
            // one the block skipped); a translation that itself starts with a
            // bigger number ("2024 was a year") keeps it.
            const bool looksNumbered = first >= 0 && number >= 0 && number <= expected + 2;
            plain.append(stripMarks(looksNumbered ? rest : row));
        }
        return plain;
    }
    return fail();
}

QStringList Translator::parseNumberedLinesPartial(const QString &text, int expected)
{
    if (expected <= 0)
        return {};
    const QStringList rows = answerRows(text);
    if (rows.isEmpty())
        return {};

    // The same numbered walk as the strict parser, but stopping wherever the
    // answer currently ends instead of demanding all expected lines. A partial
    // last line is still the best text the pane can show for that line.
    int first = -1;
    for (int i = 0; i < rows.size(); ++i) {
        if (rowNumber(rows.at(i), nullptr) == 1) {
            first = i;
            break;
        }
    }
    QStringList out;
    if (first >= 0) {
        for (int i = first; i < rows.size() && out.size() < expected; ++i) {
            QString rest;
            const int number = rowNumber(rows.at(i), &rest);
            if (number == out.size() + 1) {
                out.append(stripMarks(rest));
                continue;
            }
            if (number < 0) {
                if (out.isEmpty())
                    continue;
                out.last() += QLatin1Char(' ') + stripMarks(rows.at(i));
                continue;
            }
            break; // out of sequence: the model rewrote the layout
        }
        if (!out.isEmpty())
            return out;
    }

    // No usable numbering (yet): one value per row, stripping a leading number
    // that looks like a line number so the live pane never shows "1." alone.
    for (const QString &row : rows) {
        if (out.size() >= expected)
            break;
        QString rest;
        const int number = rowNumber(row, &rest);
        const bool looksNumbered = number >= 0 && number <= expected + 2;
        out.append(stripMarks(looksNumbered ? rest : row));
    }
    return out;
}

namespace {

// About four Latin characters per token, one per Han/kana/Hangul character.
// Spaces cost nothing, which is close enough for a request budget.
double tokenCost(QChar c)
{
    if (c.isSpace())
        return 0.0;
    const uint u = c.unicode();
    const bool wide = (u >= 0x3040 && u <= 0x30FF)     // hiragana, katakana
                      || (u >= 0x3400 && u <= 0x4DBF)  // CJK extension A
                      || (u >= 0x4E00 && u <= 0x9FFF)  // CJK unified
                      || (u >= 0xAC00 && u <= 0xD7AF)  // Hangul syllables
                      || (u >= 0xF900 && u <= 0xFAFF)  // CJK compatibility
                      || (u >= 0x20000 && u <= 0x2FA1F); // CJK extensions B-F
    return wide ? 1.0 : 0.25;
}

} // namespace

int Translator::estimateTokens(const QString &text)
{
    double tokens = 0.0;
    for (const QChar c : text)
        tokens += tokenCost(c);
    return int(std::ceil(tokens));
}

QStringList Translator::splitLongLine(const QString &line, int maxTokens)
{
    if (line.isEmpty())
        return {};
    if (maxTokens < 1)
        maxTokens = 1;
    if (estimateTokens(line) <= maxTokens)
        return {line};

    // Sentence boundaries first: the punctuation stays with the sentence it
    // ends, and the space after it starts the next one. A '.' inside "e.g." or
    // "3.14" has no space after it, so it does not cut.
    QStringList sentences;
    QString current;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        current.append(c);
        const bool asciiEnd = (c == QLatin1Char('.') || c == QLatin1Char('!')
                               || c == QLatin1Char('?'))
                              && (i + 1 >= line.size() || line.at(i + 1).isSpace());
        const bool cjkEnd = c == QChar(0x3002) || c == QChar(0xFF01) || c == QChar(0xFF1F)
                            || c == QChar(0xFF1B);
        const bool semi = c == QLatin1Char(';')
                          && (i + 1 >= line.size() || line.at(i + 1).isSpace());
        if (asciiEnd || cjkEnd || semi || c == QLatin1Char('\n')) {
            sentences.append(current);
            current.clear();
        }
    }
    if (!current.isEmpty())
        sentences.append(current);
    if (sentences.isEmpty())
        sentences.append(line);

    // Group whole sentences greedily, then hard-split the ones that are still
    // over budget (a single run-on sentence with no boundary at all).
    QStringList parts;
    QString part;
    const auto flush = [&] {
        if (!part.trimmed().isEmpty())
            parts.append(part.trimmed());
        part.clear();
    };
    for (const QString &sentence : sentences) {
        if (part.isEmpty()) {
            part = sentence;
            continue;
        }
        const QString candidate = part + sentence;
        if (estimateTokens(candidate) > maxTokens) {
            flush();
            part = sentence;
        } else {
            part = candidate;
        }
    }
    flush();

    QStringList out;
    for (const QString &piece : parts) {
        if (estimateTokens(piece) <= maxTokens) {
            out.append(piece);
            continue;
        }
        int start = 0;
        while (start < piece.size()) {
            double tokens = 0.0;
            int end = start;
            while (end < piece.size()) {
                const double cost = tokenCost(piece.at(end));
                if (tokens + cost > maxTokens && end > start)
                    break;
                tokens += cost;
                ++end;
            }
            if (end == start)
                end = start + 1; // a single character can exceed the budget
            int cut = end;
            if (cut < piece.size()) {
                int space = cut;
                while (space > start && !piece.at(space).isSpace())
                    --space;
                if (space > start && space < piece.size())
                    cut = space;
                // Never split a surrogate pair.
                if (cut > start && piece.at(cut - 1).isHighSurrogate())
                    --cut;
                if (cut <= start)
                    cut = end;
            }
            const QString value = piece.mid(start, cut - start).trimmed();
            if (!value.isEmpty())
                out.append(value);
            start = cut;
            while (start < piece.size() && piece.at(start).isSpace())
                ++start;
        }
    }
    if (out.isEmpty())
        out.append(line);
    return out;
}

QVector<QStringList> Translator::chunkTexts(const QStringList &texts, int maxTextTokens,
                                            int maxItems)
{
    QVector<QStringList> chunks;
    if (maxTextTokens < 1)
        maxTextTokens = 1;
    if (maxItems < 1)
        maxItems = 1;
    QStringList current;
    int tokens = 0;
    for (const QString &text : texts) {
        // The numbered prompt and "N. " cost a few tokens per line; count them
        // so a page of short lines cannot slip past the budget.
        const int add = estimateTokens(text) + 4;
        if (!current.isEmpty() && (current.size() >= maxItems || tokens + add > maxTextTokens)) {
            chunks.append(current);
            current.clear();
            tokens = 0;
        }
        current.append(text);
        tokens += add;
    }
    if (!current.isEmpty())
        chunks.append(current);
    return chunks;
}

QString Translator::parseResponse(const QByteArray &body, QString *error)
{
    if (error)
        error->clear();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = Translator::tr("The translation service returned an unexpected response");
        return {};
    }
    const QJsonObject root = doc.object();
    const QJsonValue errorValue = root.value(QStringLiteral("error"));
    if (!errorValue.isUndefined() && !errorValue.isNull()) {
        QString message = errorValue.toObject().value(QStringLiteral("message")).toString();
        if (message.isEmpty())
            message = errorValue.toString();
        if (error)
            *error = message.isEmpty() ? Translator::tr("The translation service returned an error") : message;
        return {};
    }
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        if (error)
            *error = Translator::tr("The translation service returned an unexpected response");
        return {};
    }
    const QJsonObject choice = choices.first().toObject();
    QString text = choice.value(QStringLiteral("message")).toObject()
                       .value(QStringLiteral("content")).toString();
    if (text.isEmpty())
        text = choice.value(QStringLiteral("text")).toString();
    text = text.trimmed();
    if (text.isEmpty()) {
        if (error)
            *error = Translator::tr("The translation service returned an empty result");
        return {};
    }
    return text;
}

QString Translator::parseStreamDelta(const QJsonObject &chunk, QString *error)
{
    if (error)
        error->clear();
    const QJsonValue errorValue = chunk.value(QStringLiteral("error"));
    if (!errorValue.isUndefined() && !errorValue.isNull()) {
        QString message = errorValue.toObject().value(QStringLiteral("message")).toString();
        if (message.isEmpty())
            message = errorValue.toString();
        if (error)
            *error = message.isEmpty() ? tr("The translation service returned an error") : message;
        return {};
    }
    const QJsonArray choices = chunk.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
        return {};
    const QJsonObject choice = choices.first().toObject();
    // The OpenAI streaming shape, then the full-message shape some compatible
    // servers send per frame instead, then the legacy completions shape.
    QString text = choice.value(QStringLiteral("delta")).toObject()
                       .value(QStringLiteral("content")).toString();
    if (text.isEmpty())
        text = choice.value(QStringLiteral("message")).toObject()
                   .value(QStringLiteral("content")).toString();
    if (text.isEmpty())
        text = choice.value(QStringLiteral("text")).toString();
    return text;
}

QString Translator::takeSseDeltas(QByteArray &buffer, bool *finished, QString *error)
{
    if (finished)
        *finished = false;
    if (error)
        error->clear();
    QString out;
    int newline = -1;
    while ((newline = buffer.indexOf('\n')) >= 0) {
        QByteArray line = buffer.left(newline);
        buffer.remove(0, newline + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(':') || trimmed.startsWith("event:"))
            continue;
        QByteArray payload;
        if (trimmed.startsWith("data:"))
            payload = trimmed.mid(5).trimmed();
        else if (trimmed.startsWith('{'))
            payload = trimmed; // NDJSON-style stream without the `data:` prefix
        else
            continue;
        if (payload == "[DONE]") {
            if (finished)
                *finished = true;
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        // `finish_reason` is the other end-of-answer marker some servers send
        // instead of, or before, [DONE].
        const QJsonArray choices = doc.object().value(QStringLiteral("choices")).toArray();
        if (finished && !choices.isEmpty()) {
            const QJsonObject choice = choices.first().toObject();
            if (choice.value(QStringLiteral("finish_reason")).isString())
                *finished = true;
        }
        QString frameError;
        const QString delta = parseStreamDelta(doc.object(), &frameError);
        if (!frameError.isEmpty()) {
            if (error)
                *error = frameError;
            continue;
        }
        out += delta;
    }
    return out;
}

bool Translator::configured() const
{
    if (!m_settings)
        return false;
    const QUrl url = endpointUrl(m_settings->translationEndpoint());
    return !m_settings->translationModel().trimmed().isEmpty() && url.isValid()
           && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"));
}

QNetworkAccessManager *Translator::nam()
{
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    return m_nam;
}

void Translator::cancel()
{
    ++m_seq;
    if (QNetworkReply *reply = m_reply) {
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
}

namespace {

QJsonObject chatPayload(const QString &model, const QString &system, const QString &text,
                        bool stream)
{
    const QJsonObject systemMessage{{QStringLiteral("role"), QStringLiteral("system")},
                                    {QStringLiteral("content"), system}};
    const QJsonObject user{{QStringLiteral("role"), QStringLiteral("user")},
                           {QStringLiteral("content"), text}};
    return QJsonObject{{QStringLiteral("model"), model},
                       {QStringLiteral("messages"), QJsonArray{systemMessage, user}},
                       {QStringLiteral("temperature"), 0},
                       {QStringLiteral("stream"), stream}};
}

QNetworkRequest chatRequest(const QUrl &url, const QString &key, bool stream,
                            int timeoutMs = 90000)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", stream ? "text/event-stream" : "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Unisic/" UNISIC_VERSION " (screenshot tool)"));
    if (!key.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    // A local model can be slow; a cloud service that has not answered or sent
    // a byte in 90 seconds is more likely stuck than thinking. The Test
    // connection probe passes its own, shorter cap.
    request.setTransferTimeout(timeoutMs);
    return request;
}

bool retryableStatus(int status)
{
    return status >= 500 || status == 408 || status == 429;
}

} // namespace

void Translator::translate(const QString &text, const QString &target, Done done)
{
    translateWith(systemPrompt(target), text, std::move(done));
}

void Translator::translateWith(const QString &system, const QString &text, Done done)
{
    if (!m_settings || !configured()) {
        done({}, Translator::tr("Set the translation endpoint and model in Settings > General"), false);
        return;
    }
    translateWithUrl(endpointUrl(m_settings->translationEndpoint()),
                     m_settings->translationModel().trimmed(),
                     m_settings->translationApiKey().trimmed(), system, text, std::move(done));
}

void Translator::translateWithUrl(const QUrl &url, const QString &model, const QString &key,
                                  const QString &system, const QString &text, Done done)
{
    if (!url.isValid()
        || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
        done({}, Translator::tr("Set the translation endpoint and model in Settings > General"), false);
        return;
    }
    cancel();
    const quint64 seq = ++m_seq;

    const QNetworkRequest request = chatRequest(url, key, false);
    QNetworkReply *reply = nam()->post(
        request, QJsonDocument(chatPayload(model, system, text, false)).toJson(QJsonDocument::Compact));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, done, seq] {
        if (seq != m_seq) {
            // cancel() already aborted this reply and owns its deletion.
            return;
        }
        m_reply = nullptr;
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply->error();
        const QString errorString = reply->errorString();
        reply->deleteLater();
        QString serviceError;
        const QString translation = parseResponse(body, &serviceError);
        if (status >= 400) {
            if (serviceError.isEmpty())
                serviceError = errorString;
            done({}, Translator::tr("Translation failed: %1").arg(serviceError),
                 retryableStatus(status));
            return;
        }
        if (networkError != QNetworkReply::NoError) {
            done({}, Translator::tr("Translation failed: %1").arg(errorString), true);
            return;
        }
        if (!serviceError.isEmpty()) {
            done({}, Translator::tr("Translation failed: %1").arg(serviceError), false);
            return;
        }
        done(translation, {}, false);
    });
}

void Translator::testConnection(const QString &target, int timeoutMs, TestDone done)
{
    if (!m_settings || !configured()) {
        done({}, Translator::tr("Set the translation endpoint and model in Settings > General"), 0);
        return;
    }
    testConnectionTo(endpointUrl(m_settings->translationEndpoint()),
                     m_settings->translationModel().trimmed(),
                     m_settings->translationApiKey().trimmed(), target, timeoutMs,
                     std::move(done));
}

void Translator::testConnectionTo(const QUrl &url, const QString &model, const QString &key,
                                  const QString &target, int timeoutMs, TestDone done)
{
    if (!url.isValid()
        || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
        done({}, Translator::tr("Set the translation endpoint and model in Settings > General"), 0);
        return;
    }
    if (timeoutMs <= 0)
        timeoutMs = 10000;
    cancel();
    const quint64 seq = ++m_seq;

    // A plain buffered request: the button is a connection check, not a
    // translation, so there is nothing to stream and nothing to parse as SSE.
    const QNetworkRequest request = chatRequest(url, key, false, timeoutMs);
    const QByteArray payload =
        QJsonDocument(chatPayload(model, systemPrompt(target), QStringLiteral("Hello"), false))
            .toJson(QJsonDocument::Compact);
    QElapsedTimer clock;
    clock.start();
    QNetworkReply *reply = nam()->post(request, payload);
    m_reply = reply;

    // setTransferTimeout only fires while nothing arrives; a server that
    // trickles a byte every few seconds would still hold the button forever.
    // The probe therefore gets a hard cap of its own.
    auto timedOut = std::make_shared<bool>(false);
    auto *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    timeout->setInterval(timeoutMs);
    connect(timeout, &QTimer::timeout, reply, [reply, timedOut] {
        *timedOut = true;
        reply->abort();
    });
    timeout->start();

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, done, seq, timedOut, clock, timeoutMs] {
                if (seq != m_seq) {
                    // cancel() already aborted this reply and owns its deletion.
                    return;
                }
                m_reply = nullptr;
                const qint64 elapsed = clock.elapsed();
                const QByteArray body = reply->readAll();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const auto networkError = reply->error();
                const QString errorString = reply->errorString();
                reply->deleteLater();
                if (*timedOut) {
                    done({}, Translator::tr("The service did not answer within %1 seconds")
                                 .arg(timeoutMs / 1000),
                         elapsed);
                    return;
                }
                QString serviceError;
                const QString answer = parseResponse(body, &serviceError);
                if (status >= 400) {
                    if (serviceError.isEmpty()) {
                        // No error object to quote: show the status and the
                        // body's own text, not a paraphrase.
                        QString text = QString::fromUtf8(body).trimmed();
                        if (text.size() > 300)
                            text = text.left(300) + QStringLiteral("...");
                        serviceError = text.isEmpty()
                                           ? QStringLiteral("HTTP %1").arg(status)
                                           : QStringLiteral("HTTP %1: %2").arg(status).arg(text);
                    }
                    done({}, serviceError, elapsed);
                    return;
                }
                if (networkError != QNetworkReply::NoError) {
                    done({}, errorString, elapsed);
                    return;
                }
                if (!serviceError.isEmpty()) {
                    done({}, serviceError, elapsed);
                    return;
                }
                done(answer, {}, elapsed);
            });
}

void Translator::translateStreaming(const QString &system, const QString &text, Delta delta,
                                    Done done)
{
    if (!m_settings || !configured()) {
        done({}, Translator::tr("Set the translation endpoint and model in Settings > General"), false);
        return;
    }
    translateStreamingTo(endpointUrl(m_settings->translationEndpoint()),
                         m_settings->translationModel().trimmed(),
                         m_settings->translationApiKey().trimmed(), system, text,
                         std::move(delta), std::move(done));
}

void Translator::translateStreamingTo(const QUrl &url, const QString &model, const QString &key,
                                      const QString &system, const QString &text, Delta delta,
                                      Done done)
{
    if (!url.isValid()
        || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
        done({}, Translator::tr("Set the translation endpoint and model in Settings > General"), false);
        return;
    }
    cancel();
    const quint64 seq = ++m_seq;

    const QNetworkRequest request = chatRequest(url, key, true);
    QNetworkReply *reply = nam()->post(
        request, QJsonDocument(chatPayload(model, system, text, true)).toJson(QJsonDocument::Compact));
    m_reply = reply;

    // Everything the reply has sent so far. `raw` is kept whole for the
    // buffered fallback (an endpoint that ignored `stream` and answered with
    // one JSON body), while `buffer` is consumed line by line for SSE frames.
    struct StreamState {
        QByteArray raw;
        QByteArray buffer;
        QString text;
        QString error;
        bool sawDone = false;
    };
    auto state = std::make_shared<StreamState>();

    connect(reply, &QNetworkReply::readyRead, this, [this, reply, delta, state, seq] {
        if (seq != m_seq)
            return;
        const QByteArray data = reply->readAll();
        state->raw += data;
        state->buffer += data;
        // A frame error has to survive the next read: takeSseDeltas clears its
        // error argument, so it gets a local one and the state keeps the last
        // non-empty message.
        QString frameError;
        bool finished = false;
        const QString deltas = Translator::takeSseDeltas(state->buffer, &finished, &frameError);
        state->sawDone = state->sawDone || finished;
        if (!frameError.isEmpty())
            state->error = frameError;
        if (deltas.isEmpty())
            return;
        state->text += deltas;
        if (delta)
            delta(deltas);
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, done, delta, state, seq, system, text, url, model, key] {
        if (seq != m_seq)
            return;
        m_reply = nullptr;
        if (!state->buffer.isEmpty()) {
            // A final frame without a trailing newline still counts.
            state->buffer += '\n';
            QString frameError;
            bool finished = false;
            const QString deltas = Translator::takeSseDeltas(state->buffer, &finished, &frameError);
            state->sawDone = state->sawDone || finished;
            if (!frameError.isEmpty())
                state->error = frameError;
            if (!deltas.isEmpty()) {
                state->text += deltas;
                if (delta)
                    delta(deltas);
            }
        }
        const QByteArray body = state->raw + reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply->error();
        const QString errorString = reply->errorString();
        reply->deleteLater();
        if (status >= 400) {
            // A few servers answer 400/404/405/415/422 to `stream: true` and
            // work without it. Nothing has been shown yet, so retry buffered
            // instead of making the user find out which dialect the service
            // speaks. (The body may have been read as an NDJSON error frame,
            // so this comes before the frame-error branch below.)
            const bool streamRejected = state->text.isEmpty()
                                        && (status == 400 || status == 404 || status == 405
                                            || status == 415 || status == 422);
            if (streamRejected) {
                translateWithUrl(url, model, key, system, text, done);
                return;
            }
            QString serviceError;
            parseResponse(body, &serviceError);
            if (serviceError.isEmpty())
                serviceError = errorString;
            done({}, Translator::tr("Translation failed: %1").arg(serviceError),
                 retryableStatus(status));
            return;
        }
        if (!state->error.isEmpty()) {
            done({}, Translator::tr("Translation failed: %1").arg(state->error), false);
            return;
        }
        if (networkError != QNetworkReply::NoError && !state->sawDone) {
            // The connection died before the answer's own end marker: a partial
            // answer is not a translation, so let the caller retry the chunk.
            done({}, Translator::tr("Translation failed: %1").arg(errorString), true);
            return;
        }
        if (!state->text.isEmpty()) {
            // Some endpoints end the stream without [DONE] but close cleanly;
            // the text is still the whole answer.
            done(state->text, {}, false);
            return;
        }
        if (networkError != QNetworkReply::NoError && body.trimmed().isEmpty()) {
            done({}, Translator::tr("Translation failed: %1").arg(errorString), true);
            return;
        }
        // No SSE frames at all: an endpoint that ignored `stream` and sent a
        // normal chat-completions body.
        QString serviceError;
        const QString translation = parseResponse(body, &serviceError);
        if (!serviceError.isEmpty()) {
            done({}, Translator::tr("Translation failed: %1").arg(serviceError), false);
            return;
        }
        done(translation, {}, false);
    });
}

