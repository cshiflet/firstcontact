#include "api/RestClient.h"
#include "api/Errors.h"

#include <QObject>
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QEventLoop>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <vector>

using fc::api::ApiError;
using fc::api::ApiErrorKind;
using fc::api::RestClient;

// Scripts a response per incoming request.
//   Status        — return that status with empty body and Connection: close
//   FailTransport — accept the connection then close it without writing
//
// Steps are consumed in order. After the script is exhausted, the server
// returns 200 with an empty body so test cases can assert "should not have
// retried" by counting requestsSeen instead of relying on timeouts.
class ScriptedHttpServer : public QObject {
    Q_OBJECT
public:
    enum class Action { Status, FailTransport };
    struct Step {
        Action action;
        int    status;   // ignored when action == FailTransport
    };

    ScriptedHttpServer() : server_(new QTcpServer(this)) {
        connect(server_, &QTcpServer::newConnection,
                this, &ScriptedHttpServer::onNewConnection);
    }

    bool start() {
        return server_->listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const { return server_->serverPort(); }

    void script(std::vector<Step> steps) { steps_ = std::move(steps); }
    int requestsSeen() const { return requestsSeen_; }
    QStringList authorizationHeaders() const { return authorizationHeaders_; }

private slots:
    void onNewConnection() {
        while (auto* sock = server_->nextPendingConnection()) {
            connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
                buf_ += sock->readAll();
                // Wait for at least the request line + a header terminator.
                // We don't parse — the test only cares about request count.
                if (!buf_.contains("\r\n\r\n")) return;

                authorizationHeaders_.push_back(extractAuthorization(buf_));
                const int idx = requestsSeen_++;
                const Step step = idx < int(steps_.size())
                    ? steps_[idx]
                    : Step{Action::Status, 200};

                if (step.action == Action::FailTransport) {
                    // Graceful FIN with no HTTP response written. QNAM
                    // sees the connection drop without bytes and reports
                    // RemoteHostClosedError. abort() (RST) is too abrupt
                    // — QNAM sometimes treats it as no-error for POST.
                    sock->disconnectFromHost();
                    buf_.clear();
                    return;
                }

                QByteArray resp;
                resp += "HTTP/1.1 ";
                resp += QByteArray::number(step.status);
                resp += " Test\r\n";
                resp += "Content-Length: 0\r\n";
                resp += "Connection: close\r\n";
                resp += "\r\n";
                sock->write(resp);
                sock->flush();
                sock->disconnectFromHost();
                sock->deleteLater();
                buf_.clear();
            });
            connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        }
    }

private:
    QTcpServer*        server_       = nullptr;
    std::vector<Step>  steps_;
    int                requestsSeen_ = 0;
    QByteArray         buf_;
    QStringList        authorizationHeaders_;

    static QString extractAuthorization(const QByteArray& request) {
        const QByteArray prefix = QByteArrayLiteral("Authorization:");
        const auto lines = request.split('\n');
        for (QByteArray line : lines) {
            line = line.trimmed();
            if (line.startsWith(prefix)) {
                return QString::fromLatin1(line.mid(prefix.size()).trimmed());
            }
        }
        return {};
    }
};

namespace {

QUrl urlFor(quint16 port) {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/v1/test").arg(port));
}

// Drives one RestClient::send call and pumps the event loop until the
// callback fires (or the watchdog elapses). Returns the (body, error) pair.
struct SendOutcome {
    QByteArray body;
    ApiError   err;
    bool       fired = false;
};

// Production POSTs / PATCHes always carry a payload (MIME body for
// messages.send, JSON for labels/modify, etc.). With a body present QNAM
// considers the request "in flight" and stops doing its internal
// transparent retry on connection drop — which is what we want, since
// our application-layer guard relies on seeing the transport error.
constexpr const char* kBodyForVerbsWithPayload = "{}";

SendOutcome runSend(RestClient& client, RestClient::Verb verb,
                    const QUrl& url, int watchdogMs = 30'000) {
    SendOutcome out;
    QEventLoop loop;
    QByteArray body;
    QByteArray contentType;
    if (verb == RestClient::Verb::Post || verb == RestClient::Verb::Put ||
        verb == RestClient::Verb::Patch) {
        body = QByteArray(kBodyForVerbsWithPayload);
        contentType = "application/json";
    }
    client.send(verb, url, body, contentType,
        [&](QByteArray b, ApiError e) {
            out.body = std::move(b);
            out.err  = std::move(e);
            out.fired = true;
            loop.quit();
        });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    watchdog.start(watchdogMs);
    loop.exec();
    return out;
}

}  // namespace

class TestRestClient : public QObject {
    Q_OBJECT
private slots:
    // GET hits transport failure (server closes mid-flight) once, then the
    // retry succeeds. Confirms idempotent retries on transport errors at the
    // application layer (in addition to whatever QNAM does internally).
    void getRetriesOnTransportFailure() {
        ScriptedHttpServer server;
        QVERIFY(server.start());
        server.script({
            { ScriptedHttpServer::Action::FailTransport, 0 },
            { ScriptedHttpServer::Action::Status,        200 },
        });
        RestClient client([]() { return QStringLiteral("token"); });
        const auto out = runSend(client, RestClient::Verb::Get, urlFor(server.port()));
        QVERIFY(out.fired);
        QVERIFY(out.err.isOk());
        QCOMPARE(server.requestsSeen(), 2);
    }

    // Qt 6.10 surfaces this transport failure to RestClient, so our
    // non-idempotent guard can return a Network error without re-sending.
    // Historical context: Qt 6.4 QNAM transparently retried this case below
    // the application layer, including for POST, so older runs saw a clean
    // 200 here and the server saw two requests.
    void qnamTransparentlyRetriesPostOnTransportFailure_documented() {
        ScriptedHttpServer server;
        QVERIFY(server.start());
        server.script({
            { ScriptedHttpServer::Action::FailTransport, 0 },
            { ScriptedHttpServer::Action::Status,        200 },
        });
        RestClient client([]() { return QStringLiteral("token"); });
        const auto out = runSend(client, RestClient::Verb::Post, urlFor(server.port()));
        QVERIFY(out.fired);
        QCOMPARE(out.err.kind, ApiErrorKind::Network);
        QCOMPARE(server.requestsSeen(), 1);
    }

    // GET should retry on 5xx (idempotent — server may have processed
    // partially, but a duplicate read is harmless).
    void getRetriesOn5xx() {
        ScriptedHttpServer server;
        QVERIFY(server.start());
        server.script({
            { ScriptedHttpServer::Action::Status, 503 },
            { ScriptedHttpServer::Action::Status, 200 },
        });
        RestClient client([]() { return QStringLiteral("token"); });
        const auto out = runSend(client, RestClient::Verb::Get, urlFor(server.port()));
        QVERIFY(out.fired);
        QVERIFY(out.err.isOk());
        QCOMPARE(server.requestsSeen(), 2);
    }

    // POST must NOT retry on 5xx — server may have processed the send and
    // lost the response. Caller is told once and decides what to do.
    void postDoesNotRetryOn5xx() {
        ScriptedHttpServer server;
        QVERIFY(server.start());
        server.script({
            { ScriptedHttpServer::Action::Status, 503 },
            { ScriptedHttpServer::Action::Status, 200 },
        });
        RestClient client([]() { return QStringLiteral("token"); });
        const auto out = runSend(client, RestClient::Verb::Post, urlFor(server.port()));
        QVERIFY(out.fired);
        QCOMPARE(out.err.kind, ApiErrorKind::Server);
        QCOMPARE(out.err.httpStatus, 503);
        QCOMPARE(server.requestsSeen(), 1);
    }

    // 429 means "I did NOT process your request — try again later". Safe to
    // retry on every verb, including POST.
    void postRetriesOn429() {
        ScriptedHttpServer server;
        QVERIFY(server.start());
        server.script({
            { ScriptedHttpServer::Action::Status, 429 },
            { ScriptedHttpServer::Action::Status, 200 },
        });
        RestClient client([]() { return QStringLiteral("token"); });
        const auto out = runSend(client, RestClient::Verb::Post, urlFor(server.port()));
        QVERIFY(out.fired);
        QVERIFY(out.err.isOk());
        QCOMPARE(server.requestsSeen(), 2);
    }

    void getRetriesOn429() {
        ScriptedHttpServer server;
        QVERIFY(server.start());
        server.script({
            { ScriptedHttpServer::Action::Status, 429 },
            { ScriptedHttpServer::Action::Status, 200 },
        });
        RestClient client([]() { return QStringLiteral("token"); });
        const auto out = runSend(client, RestClient::Verb::Get, urlFor(server.port()));
        QVERIFY(out.fired);
        QVERIFY(out.err.isOk());
        QCOMPARE(server.requestsSeen(), 2);
    }

    // 401 triggers a single token refresh + retry, regardless of verb.
    // The one-getter constructor preserves existing test ergonomics: its
    // default forced-refresh path calls the same scriptable getter again.
    void authRefreshFiresOnceAndRetries() {
        ScriptedHttpServer server;
        QVERIFY(server.start());
        server.script({
            { ScriptedHttpServer::Action::Status, 401 },
            { ScriptedHttpServer::Action::Status, 200 },
        });
        int tokenCalls = 0;
        RestClient client([&tokenCalls]() {
            ++tokenCalls;
            return tokenCalls == 1 ? QStringLiteral("stale")
                                   : QStringLiteral("fresh");
        });
        const auto out = runSend(client, RestClient::Verb::Post, urlFor(server.port()));
        QVERIFY(out.fired);
        QVERIFY(out.err.isOk());
        QCOMPARE(server.requestsSeen(), 2);
        QCOMPARE(tokenCalls, 2);
    }

    // A 401 must force-refresh even when the normal getter would keep
    // returning a stale-but-not-expired token. This exercises the RestClient
    // abstraction directly without needing to stand up OAuth/QtKeychain.
    void auth401UsesForcedRefreshForStaleUnexpiredToken() {
        ScriptedHttpServer server;
        QVERIFY(server.start());
        server.script({
            { ScriptedHttpServer::Action::Status, 401 },
            { ScriptedHttpServer::Action::Status, 200 },
        });

        QString currentToken = QStringLiteral("stale");
        int normalTokenCalls = 0;
        int forcedRefreshCalls = 0;
        RestClient client(
            [&]() {
                ++normalTokenCalls;
                return currentToken;
            },
            [&]() {
                ++forcedRefreshCalls;
                currentToken = QStringLiteral("fresh");
                return currentToken;
            });

        const auto out = runSend(client, RestClient::Verb::Get, urlFor(server.port()));
        QVERIFY(out.fired);
        QVERIFY(out.err.isOk());
        QCOMPARE(server.requestsSeen(), 2);
        QCOMPARE(normalTokenCalls, 1);
        QCOMPARE(forcedRefreshCalls, 1);
        QCOMPARE(server.authorizationHeaders(),
                 QStringList({QStringLiteral("Bearer stale"),
                              QStringLiteral("Bearer fresh")}));
    }

    // Two consecutive 401s — the second is NOT retried again; the caller
    // gets the auth error. Without this, a permanently revoked token would
    // loop forever.
    void authRefreshNotRetriedTwice() {
        ScriptedHttpServer server;
        QVERIFY(server.start());
        server.script({
            { ScriptedHttpServer::Action::Status, 401 },
            { ScriptedHttpServer::Action::Status, 401 },
            { ScriptedHttpServer::Action::Status, 200 },   // never reached
        });
        RestClient client([]() { return QStringLiteral("token"); });
        const auto out = runSend(client, RestClient::Verb::Get, urlFor(server.port()));
        QVERIFY(out.fired);
        QCOMPARE(out.err.kind, ApiErrorKind::Auth);
        QCOMPARE(server.requestsSeen(), 2);
    }

    // Empty token short-circuits without making any HTTP request — tests
    // that the auth-failure-before-the-wire path is wired correctly.
    void emptyTokenShortCircuits() {
        ScriptedHttpServer server;
        QVERIFY(server.start());
        server.script({});
        RestClient client([]() { return QString(); });
        const auto out = runSend(client, RestClient::Verb::Get, urlFor(server.port()));
        QVERIFY(out.fired);
        QCOMPARE(out.err.kind, ApiErrorKind::Auth);
        QCOMPARE(server.requestsSeen(), 0);
    }
};

QTEST_MAIN(TestRestClient)
#include "test_rest_client.moc"
