#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include "MockTranslationEndpoint.h"
#include "Settings.h"
#include "ocr/Translator.h"


// Pure translation plumbing: URL shapes people actually paste, the prompt, and
// chat-completions/error bodies. The streaming tests below run against a local
// mock endpoint, never a real service.
class TranslatorTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void endpointShapes();
    void promptNamesTheTarget();
    void parsesChatCompletion();
    void parsesServiceError();
    void numberedPromptKeepsOneLinePerLine();
    void parsesNumberedLines();
    void toleratesRealWorldAnswers();
    void rejectsUnnumberedTranslation();
    void parsesSseFrames();
    void partialNumberedLinesFillThePane();
    void estimatesTokensByScript();
    void splitsLongLinesAtSentenceBoundaries();
    void chunksWithinTokenBudget();
    void streamsAgainstSseServer();
    void fallsBackWhenStreamIsRejected();
    void parsesJsonBodyWhenStreamIgnored();
    void cancelsMidStream();
};

void TranslatorTest::initTestCase()
{
    // Settings is the thing Translator reads; keep every write inside the
    // QtTest sandbox instead of the developer's real config.
    QStandardPaths::setTestModeEnabled(true);
}

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

void TranslatorTest::parsesSseFrames()
{
    bool finished = false;
    QString error;
    QByteArray buffer =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n"
        ": keep-alive\n"
        "event: message\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
        "data: [DONE]\n\n";
    QCOMPARE(Translator::takeSseDeltas(buffer, &finished, &error), QStringLiteral("Hello"));
    QVERIFY(finished);
    QVERIFY(error.isEmpty());

    // A frame that has not finished arriving stays in the buffer for the next
    // read instead of being parsed half-way.
    QByteArray partial =
        "data: {\"choices\":[{\"delta\":{\"content\":\"tic\"}}]}\n";
    finished = false;
    QCOMPARE(Translator::takeSseDeltas(partial, &finished, &error), QStringLiteral("tic"));
    partial = "data: {\"choices\":[{\"delta\":{\"content\":\"cut";
    QCOMPARE(Translator::takeSseDeltas(partial, &finished, &error), QString());
    QCOMPARE(partial, QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"cut"));

    // A service error frame carries the message verbatim.
    QByteArray bad = "data: {\"error\":{\"message\":\"quota exceeded\"}}\n";
    QCOMPARE(Translator::takeSseDeltas(bad, &finished, &error), QString());
    QCOMPARE(error, QStringLiteral("quota exceeded"));

    // The NDJSON shape some compatible servers use instead of `data:` lines.
    QByteArray ndjson = "{\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n";
    error.clear();
    QCOMPARE(Translator::takeSseDeltas(ndjson, &finished, &error), QStringLiteral("Hi"));
    QVERIFY(error.isEmpty());
}

void TranslatorTest::partialNumberedLinesFillThePane()
{
    // While the answer streams, the pane can already show the lines that are
    // complete, and the partially arrived last one.
    QCOMPARE(Translator::parseNumberedLinesPartial(
                 QStringLiteral("1. Cześć\n2. Świe"), 3),
             QStringList({QStringLiteral("Cześć"), QStringLiteral("Świe")}));
    QCOMPARE(Translator::parseNumberedLinesPartial(
                 QStringLiteral("1. One\nthat wrapped\n2. Tw"), 3),
             QStringList({QStringLiteral("One that wrapped"), QStringLiteral("Tw")}));
    // No numbering yet: the live pane still shows the rows it has, without a
    // stray "1." marker if one arrives with the text.
    QCOMPARE(Translator::parseNumberedLinesPartial(QStringLiteral("Hello"), 2),
             QStringList({QStringLiteral("Hello")}));
    QVERIFY(Translator::parseNumberedLinesPartial(QString(), 2).isEmpty());
    // The partial parser never claims a finished answer: fewer than expected
    // is normal, and the strict parser is what decides success.
    QCOMPARE(Translator::parseNumberedLinesPartial(
                 QStringLiteral("1. One\n2. Two"), 3).size(), 2);
}

void TranslatorTest::estimatesTokensByScript()
{
    QCOMPARE(Translator::estimateTokens(QString()), 0);
    // About four Latin characters per token; spaces and punctuation cost a
    // little, so this is the plan's "about 4", not a byte count.
    QCOMPARE(Translator::estimateTokens(QStringLiteral("Hello world")), 3);
    // One per Han character, as the budget in the plan says.
    QCOMPARE(Translator::estimateTokens(QStringLiteral("你好世界")), 4);
    QCOMPARE(Translator::estimateTokens(QStringLiteral("Hello 你好")), 4);
    QVERIFY(Translator::estimateTokens(QString(2000, QLatin1Char('a'))) > 400);
}

void TranslatorTest::splitsLongLinesAtSentenceBoundaries()
{
    QCOMPARE(Translator::splitLongLine(QStringLiteral("Short line."), 50),
             QStringList({QStringLiteral("Short line.")}));
    QVERIFY(Translator::splitLongLine(QString(), 50).isEmpty());

    const QString line = QStringLiteral(
        "The first sentence is long enough. The second one is here! Is the third one "
        "there? And the fourth closes it.");
    const QStringList parts = Translator::splitLongLine(line, 12);
    QVERIFY(parts.size() >= 2);
    for (const QString &part : parts)
        QVERIFY2(Translator::estimateTokens(part) <= 12, qPrintable(part));
    // Splitting must not lose or reorder text: only whitespace at the cuts is
    // normalized, which the strings around the punctuation already carry.
    QCOMPARE(parts.join(QStringLiteral(" ")).simplified(), line.simplified());

    // One run-on sentence with no punctuation: split at word boundaries, never
    // mid-word when a space is available.
    QString runOn;
    for (int i = 0; i < 80; ++i)
        runOn += QStringLiteral("word ") + QString::number(i) + QLatin1Char(' ');
    const QStringList words = Translator::splitLongLine(runOn.trimmed(), 20);
    QVERIFY(words.size() > 1);
    for (const QString &part : words)
        QVERIFY2(Translator::estimateTokens(part) <= 20, qPrintable(part));
    QCOMPARE(words.join(QStringLiteral(" ")).simplified(), runOn.simplified());

    // CJK has no spaces to cut at: a hard cut still has to stay under budget.
    QString cjk;
    for (int i = 0; i < 60; ++i)
        cjk += QStringLiteral("你好世界");
    const QStringList cjkParts = Translator::splitLongLine(cjk, 10);
    QVERIFY(cjkParts.size() > 1);
    for (const QString &part : cjkParts)
        QVERIFY2(Translator::estimateTokens(part) <= 10, qPrintable(part));
    QCOMPARE(cjkParts.join(QString()), cjk);
}

void TranslatorTest::chunksWithinTokenBudget()
{
    // 95 short lines still split by the line cap first, so a page keeps the
    // same request count it had before the token budget existed.
    QStringList lines;
    for (int i = 0; i < 95; ++i)
        lines.append(QStringLiteral("line %1").arg(i));
    QCOMPARE(Translator::chunkTexts(lines, Translator::kRequestTokenBudget, 40).size(), 3);

    // A tight budget splits earlier even with the line cap far away.
    const QVector<QStringList> tight =
        Translator::chunkTexts({QStringLiteral("aaaa"), QStringLiteral("bbbb")}, 9, 40);
    QCOMPARE(tight.size(), 2);
    QCOMPARE(tight.at(0), QStringList({QStringLiteral("aaaa")}));
    // One entry over the budget still gets a chunk of its own rather than
    // being dropped or silently truncated.
    const QVector<QStringList> oversized =
        Translator::chunkTexts({QString(500, QLatin1Char('x'))}, 10);
    QCOMPARE(oversized.size(), 1);
    QCOMPARE(oversized.at(0).size(), 1);
    QVERIFY(Translator::chunkTexts({}, 10).isEmpty());
}

void TranslatorTest::streamsAgainstSseServer()
{
    MockTranslationEndpoint server(MockTranslationEndpoint::Sse);
    QVERIFY(server.start());
    Settings settings;
    settings.setTranslationEndpoint(server.base());
    settings.setTranslationModel(QStringLiteral("mock"));
    Translator translator(&settings);

    QStringList deltas;
    QString result;
    QString error;
    bool retryable = true;
    bool done = false;
    translator.translateStreaming(
        QStringLiteral("Translate."), QStringLiteral("Hello"),
        [&deltas](const QString &partial) { deltas.append(partial); },
        [&](const QString &text, const QString &failure, bool retry) {
            done = true;
            result = text;
            error = failure;
            retryable = retry;
        });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(error, QString());
    QCOMPARE(result, QStringLiteral("Hello world"));
    QCOMPARE(deltas.join(QString()), QStringLiteral("Hello world"));
    QVERIFY(!retryable);
    QCOMPARE(server.bodies.size(), 1);
    QVERIFY(server.bodies.first().contains("\"stream\":true"));
}

void TranslatorTest::fallsBackWhenStreamIsRejected()
{
    MockTranslationEndpoint server(MockTranslationEndpoint::RejectStream);
    QVERIFY(server.start());
    Settings settings;
    settings.setTranslationEndpoint(server.base());
    settings.setTranslationModel(QStringLiteral("mock"));
    Translator translator(&settings);

    QStringList deltas;
    QString result;
    QString error;
    bool done = false;
    translator.translateStreaming(
        QStringLiteral("Translate."), QStringLiteral("Hello"),
        [&deltas](const QString &partial) { deltas.append(partial); },
        [&](const QString &text, const QString &failure, bool) {
            done = true;
            result = text;
            error = failure;
        });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(error, QString());
    QCOMPARE(result, QStringLiteral("Buffered answer"));
    QVERIFY(deltas.isEmpty());
    // One streaming attempt, then the buffered retry the user never sees.
    QCOMPARE(server.bodies.size(), 2);
    QVERIFY(server.bodies.at(0).contains("\"stream\":true"));
    QVERIFY(server.bodies.at(1).contains("\"stream\":false"));
}

void TranslatorTest::parsesJsonBodyWhenStreamIgnored()
{
    MockTranslationEndpoint server(MockTranslationEndpoint::Json);
    QVERIFY(server.start());
    Settings settings;
    settings.setTranslationEndpoint(server.base());
    settings.setTranslationModel(QStringLiteral("mock"));
    Translator translator(&settings);

    QString result;
    QString error;
    bool done = false;
    translator.translateStreaming(
        QStringLiteral("Translate."), QStringLiteral("Hello"), {},
        [&](const QString &text, const QString &failure, bool) {
            done = true;
            result = text;
            error = failure;
        });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(error, QString());
    QCOMPARE(result, QStringLiteral("Buffered answer"));
}

void TranslatorTest::cancelsMidStream()
{
    MockTranslationEndpoint server(MockTranslationEndpoint::SlowSse);
    QVERIFY(server.start());
    Settings settings;
    settings.setTranslationEndpoint(server.base());
    settings.setTranslationModel(QStringLiteral("mock"));
    Translator translator(&settings);

    QStringList deltas;
    bool done = false;
    translator.translateStreaming(
        QStringLiteral("Translate."), QStringLiteral("Hello"),
        [&deltas](const QString &partial) { deltas.append(partial); },
        [&done](const QString &, const QString &, bool) { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(!deltas.isEmpty(), 3000);
    translator.cancel();
    // The rest of the server's answer and its [DONE] must not reach the
    // callback after Cancel.
    QTest::qWait(700);
    QVERIFY(!done);
    QCOMPARE(deltas.join(QString()), QStringLiteral("Hello"));
}

QTEST_GUILESS_MAIN(TranslatorTest)
#include "TranslatorTest.moc"
