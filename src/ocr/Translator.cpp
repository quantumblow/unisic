#include "Translator.h"

#include "Settings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

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

QString Translator::systemPrompt(const QString &target)
{
    return QStringLiteral(
               "You are a translation engine. Translate the user's text into %1. "
               "Keep the original line breaks, numbers, punctuation and order. "
               "Return only the translation, with no explanation, notes or quotation marks.")
        .arg(target);
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

void Translator::translate(const QString &text, const QString &target, Done done)
{
    if (!m_settings || !configured()) {
        done({}, Translator::tr("Set the translation endpoint and model in Settings > General"));
        return;
    }
    const QUrl url = endpointUrl(m_settings->translationEndpoint());
    const QString model = m_settings->translationModel().trimmed();
    const QString key = m_settings->translationApiKey().trimmed();
    cancel();
    const quint64 seq = ++m_seq;

    const QJsonObject system{{QStringLiteral("role"), QStringLiteral("system")},
                             {QStringLiteral("content"), systemPrompt(target)}};
    const QJsonObject user{{QStringLiteral("role"), QStringLiteral("user")},
                           {QStringLiteral("content"), text}};
    const QJsonObject payload{{QStringLiteral("model"), model},
                              {QStringLiteral("messages"), QJsonArray{system, user}},
                              {QStringLiteral("temperature"), 0},
                              {QStringLiteral("stream"), false}};

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Unisic/" UNISIC_VERSION " (screenshot tool)"));
    if (!key.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    // A local model can be slow; a cloud service that has not answered in 90
    // seconds is more likely stuck than thinking.
    request.setTransferTimeout(90000);

    QNetworkReply *reply = nam()->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, done, seq] {
        if (seq != m_seq) {
            // cancel() already aborted this reply and owns its deletion.
            return;
        }
        m_reply = nullptr;
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString serviceError;
        const QString translation = parseResponse(body, &serviceError);
        reply->deleteLater();
        if (status >= 400) {
            if (serviceError.isEmpty())
                serviceError = reply->errorString();
            done({}, Translator::tr("Translation failed: %1").arg(serviceError));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            done({}, Translator::tr("Translation failed: %1").arg(reply->errorString()));
            return;
        }
        if (!serviceError.isEmpty()) {
            done({}, Translator::tr("Translation failed: %1").arg(serviceError));
            return;
        }
        done(translation, {});
    });
}
