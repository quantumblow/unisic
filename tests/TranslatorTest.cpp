#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>

#include "ocr/Translator.h"

// Pure translation plumbing: URL shapes people actually paste, the prompt, and
// chat-completions/error bodies. No network request is made.
class TranslatorTest : public QObject
{
    Q_OBJECT

private slots:
    void endpointShapes();
    void promptNamesTheTarget();
    void parsesChatCompletion();
    void parsesServiceError();
};

void TranslatorTest::endpointShapes()
{
    QCOMPARE(Translator::endpointUrl(QStringLiteral("https://api.openai.com/v1")).toString(),
             QStringLiteral("https://api.openai.com/v1/chat/completions"));
    QCOMPARE(Translator::endpointUrl(QStringLiteral("https://api.openai.com/v1/")).toString(),
             QStringLiteral("https://api.openai.com/v1/chat/completions"));
    QCOMPARE(Translator::endpointUrl(QStringLiteral("http://localhost:11434")).toString(),
             QStringLiteral("http://localhost:11434/v1/chat/completions"));
    QCOMPARE(Translator::endpointUrl(
                 QStringLiteral("https://api.example.test/v1/chat/completions")).toString(),
             QStringLiteral("https://api.example.test/v1/chat/completions"));
    QVERIFY(Translator::endpointUrl(QString()).isEmpty());
}

void TranslatorTest::promptNamesTheTarget()
{
    const QString prompt = Translator::systemPrompt(QStringLiteral("Simplified Chinese"));
    QVERIFY(prompt.contains(QStringLiteral("Simplified Chinese")));
    QVERIFY(prompt.contains(QStringLiteral("only the translation")));
}

void TranslatorTest::parsesChatCompletion()
{
    const QByteArray body = R"({"choices":[{"message":{"content":"你好，世界"}}]})";
    QString error;
    QCOMPARE(Translator::parseResponse(body, &error), QStringLiteral("你好，世界"));
    QVERIFY(error.isEmpty());
}

void TranslatorTest::parsesServiceError()
{
    const QByteArray body = R"({"error":{"message":"invalid api key"}})";
    QString error;
    QCOMPARE(Translator::parseResponse(body, &error), QString());
    QCOMPARE(error, QStringLiteral("invalid api key"));

    error.clear();
    QCOMPARE(Translator::parseResponse("not json", &error), QString());
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(TranslatorTest)
#include "TranslatorTest.moc"
