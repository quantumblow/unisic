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
    void numberedPromptKeepsOneLinePerLine();
    void parsesNumberedLines();
    void toleratesRealWorldAnswers();
    void rejectsUnnumberedTranslation();
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

void TranslatorTest::numberedPromptKeepsOneLinePerLine()
{
    QVERIFY(Translator::numberedPrompt(QStringLiteral("Polish")).contains(QStringLiteral("Polish")));
    QVERIFY(Translator::numberedPrompt(QStringLiteral("Polish"))
                .contains(QStringLiteral("same number of lines")));
    QCOMPARE(Translator::numberedRequest({QStringLiteral("Hi"), QStringLiteral("Two")}),
             QStringLiteral("1. Hi\n2. Two"));
    QCOMPARE(Translator::numberedRequest({}), QString());
}

void TranslatorTest::parsesNumberedLines()
{
    QString error;
    QCOMPARE(Translator::parseNumberedLines(QStringLiteral("1. Cześć\n2. Świecie"), 2, &error),
             QStringList({QStringLiteral("Cześć"), QStringLiteral("Świecie")}));
    QVERIFY(error.isEmpty());

    // Markdown emphasis, "[1]" style and a blank spacer line all read the same.
    QCOMPARE(Translator::parseNumberedLines(QStringLiteral("**1.** One\n\n[2] Two"), 2, &error),
             QStringList({QStringLiteral("One"), QStringLiteral("Two")}));
    QVERIFY(error.isEmpty());

    // A line whose translation happens to start with a number keeps its text.
    QCOMPARE(Translator::parseNumberedLines(QStringLiteral("1. 2024 was a year"), 1, &error),
             QStringList({QStringLiteral("2024 was a year")}));
    QVERIFY(error.isEmpty());
}

void TranslatorTest::rejectsUnnumberedTranslation()
{
    QString error;
    // One line where two were asked for: a count mismatch, no guessing.
    QVERIFY(Translator::parseNumberedLines(QStringLiteral("Cześć świecie"), 2, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    // Two input lines merged into one answer: a count mismatch, so the caller
    // falls back to the whole-text translation instead of guessing.
    error.clear();
    QVERIFY(Translator::parseNumberedLines(QStringLiteral("1. One two"), 2, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    // A skipped number still has one answer line per input line: map it by
    // position and drop the number (the picture must not show "3." either).
    error.clear();
    QCOMPARE(Translator::parseNumberedLines(QStringLiteral("1. One\n3. Three"), 2, &error),
             QStringList({QStringLiteral("One"), QStringLiteral("Three")}));
    QVERIFY(error.isEmpty());
}

void TranslatorTest::toleratesRealWorldAnswers()
{
    QString error;
    // A sentence before the numbered block is a preamble, not a translation.
    QCOMPARE(Translator::parseNumberedLines(
                 QStringLiteral("Here is the translation:\n1. One\n2. Two"), 2, &error),
             QStringList({QStringLiteral("One"), QStringLiteral("Two")}));
    QVERIFY(error.isEmpty());

    // A closing sentence after the last line is noise.
    QCOMPARE(Translator::parseNumberedLines(
                 QStringLiteral("1. One\n2. Two\nHope this helps."), 2, &error),
             QStringList({QStringLiteral("One"), QStringLiteral("Two")}));
    QVERIFY(error.isEmpty());

    // Code fences around the answer.
    QCOMPARE(Translator::parseNumberedLines(
                 QStringLiteral("```\n1. One\n2. Two\n```"), 2, &error),
             QStringList({QStringLiteral("One"), QStringLiteral("Two")}));

    // A wrapped translation line continues the previous one instead of
    // breaking the block.
    QCOMPARE(Translator::parseNumberedLines(
                 QStringLiteral("1. One long line\nthat wrapped\n2. Two"), 2, &error),
             QStringList({QStringLiteral("One long line that wrapped"), QStringLiteral("Two")}));

    // One line per input line but no numbering at all: map positionally.
    QCOMPARE(Translator::parseNumberedLines(QStringLiteral("One\nTwo\nThree"), 3, &error),
             QStringList({QStringLiteral("One"), QStringLiteral("Two"), QStringLiteral("Three")}));
    QVERIFY(error.isEmpty());

    // Numbering the block cannot use (a restart, a 0-based list) still maps by
    // position when the count matches, and the numbers do not reach the picture.
    QCOMPARE(Translator::parseNumberedLines(QStringLiteral("0. One\n1. Two"), 2, &error),
             QStringList({QStringLiteral("One"), QStringLiteral("Two")}));
    QVERIFY(error.isEmpty());
}

QTEST_GUILESS_MAIN(TranslatorTest)
#include "TranslatorTest.moc"
