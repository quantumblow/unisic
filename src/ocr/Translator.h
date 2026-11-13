#pragma once
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <functional>

class Settings;
class QNetworkAccessManager;
class QNetworkReply;

// OpenAI-compatible chat-completions translation. One request at a time; the
// OCR text is sent only when the user presses Translate. Works with OpenAI and
// any compatible server, including a local Ollama (`http://localhost:11434/v1`)
// where nothing leaves the machine.
class Translator : public QObject
{
    Q_OBJECT

public:
    // `retryable` is true for a transport error or a 5xx (and for the timeout /
    // rate-limit statuses), false for a request or answer the service refused:
    // retrying those would only send the same text twice.
    using Done = std::function<void(const QString &translation, const QString &error,
                                    bool retryable)>;
    // One piece of an answer as it streams in. Called on the GUI thread, in
    // order, and never after Done.
    using Delta = std::function<void(const QString &partial)>;
    // Test connection: the answer and the round trip, or the service's own
    // error text (never wrapped in our own wording) and the time it took.
    using TestDone = std::function<void(const QString &answer, const QString &error,
                                        qint64 elapsedMs)>;
    explicit Translator(Settings *settings = nullptr, QObject *parent = nullptr);
    ~Translator() override;

    bool configured() const;
    // Base URLs are accepted in the shape people paste them: a bare host, a
    // /v1 base, or the full /chat/completions URL.
    static QUrl endpointUrl(const QString &base);
    // True when an endpoint stays on this machine: 127.0.0.1, localhost or
    // ::1 in any letter case. The first-send privacy notice never asks for
    // these and asks once for every other host.
    static bool isLocalEndpoint(const QUrl &url);
    static QString systemPrompt(const QString &target);
    // Content from a chat-completions body; `error` carries the service's own
    // message (or a shape error) when there is no text.
    static QString parseResponse(const QByteArray &body, QString *error);

    // Line-by-line translation, used when the result window has line boxes to
    // put a translation on. The prompt asks for one numbered output line per
    // numbered input line, so the answer can be mapped back one box at a time;
    // a model that ignores that is detected by parseNumberedLines() and the
    // caller falls back to the whole text.
    static QString numberedPrompt(const QString &target);
    static QString numberedRequest(const QStringList &lines);
    // `expected` numbered lines or nothing, with `error` explaining a count or
    // numbering mismatch. Empty and blank lines are dropped before counting.
    static QStringList parseNumberedLines(const QString &text, int expected, QString *error);
    // Whatever prefix of the answer already maps onto lines 1..expected, for
    // the live pane while a numbered chunk streams. Never counts as a finished
    // answer: the strict parser above decides that.
    static QStringList parseNumberedLinesPartial(const QString &text, int expected);

    // Rough token budget, in one place: about four characters per token for
    // Latin text and one per Han/kana/Hangul character. Used to keep one
    // request inside a small model's context, not to bill anything.
    static int estimateTokens(const QString &text);
    // Splits `line` at sentence boundaries (then at word boundaries, and only
    // then mid-word) until every part fits `maxTokens`. Returns the line
    // unchanged when it already fits, and never drops text.
    static QStringList splitLongLine(const QString &line, int maxTokens);
    // Greedy chunks of at most `maxTextTokens` estimated tokens and `maxItems`
    // entries, preserving order. A single entry larger than the budget still
    // gets a chunk of its own; splitLongLine() is what keeps that rare.
    static QVector<QStringList> chunkTexts(const QStringList &texts, int maxTextTokens,
                                           int maxItems = 40);
    static constexpr int kRequestTokenBudget = 2000;
    static constexpr int kItemTokenBudget = 700;

    // The text a `stream: true` response frame adds (choices[0].delta.content,
    // or a message/text shape some compatible servers send instead), or an
    // empty string for a frame that only carries usage/finish_reason. `error`
    // carries a service error object.
    static QString parseStreamDelta(const QJsonObject &chunk, QString *error);
    // Consumes the complete `data:` lines in `buffer` (leaving any trailing,
    // still-incomplete line for the next read) and returns their deltas joined.
    // Non-SSE bodies are left untouched for the buffered fallback to parse.
    static QString takeSseDeltas(QByteArray &buffer, bool *finished, QString *error);

    // A request with an explicit system prompt.
    void translateWith(const QString &system, const QString &text, Done done);
    void translate(const QString &text, const QString &target, Done done);
    // Tries `stream: true` and falls back to the buffered request when the
    // endpoint rejects streaming or ignores it (a 200 JSON body with no SSE
    // frames). `delta` sees the answer arrive; `done` fires exactly once.
    void translateStreaming(const QString &system, const QString &text, Delta delta, Done done);
    // The same streaming request to an explicit endpoint/model/key, bypassing
    // Settings. The F8 and Developer checks use it so they never read or write
    // the user's translation configuration.
    void translateStreamingTo(const QUrl &url, const QString &model, const QString &key,
                              const QString &system, const QString &text, Delta delta,
                              Done done);
    // One small buffered request with the configured endpoint, key, model and
    // target, for Settings' Test connection button. `timeoutMs` is a hard cap
    // (the default is the plan's 10 s); the callback fires exactly once.
    void testConnection(const QString &target, int timeoutMs, TestDone done);
    // The same probe against an explicit endpoint, so the F8/Developer checks
    // never read or write the user's translation settings.
    void testConnectionTo(const QUrl &url, const QString &model, const QString &key,
                          const QString &target, int timeoutMs, TestDone done);
    void cancel();

private:
    QNetworkAccessManager *nam();
    void translateWithUrl(const QUrl &url, const QString &model, const QString &key,
                          const QString &system, const QString &text, Done done);
    Settings *m_settings = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    quint64 m_seq = 0;
};

