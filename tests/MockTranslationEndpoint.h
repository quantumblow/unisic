#pragma once
// A local OpenAI-compatible endpoint for the translation tests. It answers the
// same POST in every dialect a real endpoint uses for `stream: true`, so the
// streaming parser, its buffered fallback and Cancel are exercised without a
// network or an API key. Header-only because two test binaries include it.
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <memory>

class MockTranslationEndpoint : public QTcpServer
{
public:
    enum Mode {
        Sse,          // proper data: frames, [DONE], then close
        SlowSse,      // first frame now, the rest 400 ms later (for Cancel)
        Json,         // ignores `stream` and answers one chat-completions body
        RejectStream, // 400 to stream:true, a normal body to stream:false
        Always500,    // a retryable server error
    };

    explicit MockTranslationEndpoint(Mode mode, QObject *parent = nullptr)
        : QTcpServer(parent), m_mode(mode)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                auto request = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, socket,
                        [this, socket, request, handled] {
                            *request += socket->readAll();
                            const qsizetype headEnd = request->indexOf("\r\n\r\n");
                            if (headEnd < 0)
                                return;
                            static const QRegularExpression lengthRe(
                                QStringLiteral("(?im)^content-length:\\s*(\\d+)"));
                            const qsizetype bodyLength =
                                lengthRe
                                    .match(QString::fromLatin1(request->left(headEnd)))
                                    .captured(1)
                                    .toLongLong();
                            if (request->size() < headEnd + 4 + bodyLength)
                                return;
                            if (*handled)
                                return;
                            *handled = true;
                            bodies.append(*request);
                            respond(socket, *request);
                        });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool start() { return listen(QHostAddress::LocalHost); }
    QString base() const
    {
        return QStringLiteral("http://127.0.0.1:%1/v1").arg(serverPort());
    }

    QList<QByteArray> bodies;

private:
    static void writeJson(QTcpSocket *socket, int status, const QByteArray &body)
    {
        const QByteArray head =
            "HTTP/1.1 " + QByteArray::number(status)
            + (status == 200 ? QByteArray(" OK") : QByteArray(" Error"))
            + "\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
        socket->write(head);
        socket->write(body);
        socket->disconnectFromHost();
    }

    void respond(QTcpSocket *socket, const QByteArray &request)
    {
        const bool wantsStream = request.contains("\"stream\":true");
        switch (m_mode) {
        case Sse:
        case SlowSse: {
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");
            socket->write("data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n");
            const int delay = m_mode == SlowSse ? 400 : 20;
            QTimer::singleShot(delay, socket, [socket] {
                socket->write("data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
                              "data: [DONE]\n\n");
                socket->disconnectFromHost();
            });
            break;
        }
        case Json:
            writeJson(socket, 200,
                      R"({"choices":[{"message":{"content":"Buffered answer"}}]})");
            break;
        case RejectStream:
            if (wantsStream)
                writeJson(socket, 400, R"({"error":{"message":"stream is not supported"}})");
            else
                writeJson(socket, 200,
                          R"({"choices":[{"message":{"content":"Buffered answer"}}]})");
            break;
        case Always500:
            writeJson(socket, 500, R"({"error":{"message":"server is down"}})");
            break;
        }
    }

    Mode m_mode;
};
