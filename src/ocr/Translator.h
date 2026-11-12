#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
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
    using Done = std::function<void(const QString &translation, const QString &error)>;
    explicit Translator(Settings *settings = nullptr, QObject *parent = nullptr);
    ~Translator() override;

    bool configured() const;
    // Base URLs are accepted in the shape people paste them: a bare host, a
    // /v1 base, or the full /chat/completions URL.
    static QUrl endpointUrl(const QString &base);
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

    // A request with an explicit system prompt.
    void translateWith(const QString &system, const QString &text, Done done);
    void translate(const QString &text, const QString &target, Done done);
    void cancel();

private:
    QNetworkAccessManager *nam();
    Settings *m_settings = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    quint64 m_seq = 0;
};
