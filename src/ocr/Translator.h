#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
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

    void translate(const QString &text, const QString &target, Done done);
    void cancel();

private:
    QNetworkAccessManager *nam();
    Settings *m_settings = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    quint64 m_seq = 0;
};
