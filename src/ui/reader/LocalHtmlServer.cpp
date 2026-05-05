#include "LocalHtmlServer.h"

#include "util/Base64Url.h"

#include <QHostAddress>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace fc::ui {

namespace {

constexpr int kIdleTimeoutMs    = 5 * 60 * 1000;   // 5 min: snap chromium can take 30+s to register
constexpr int kPerSocketReadMs  = 5'000;
constexpr int kMaxRequestBytes  = 32 * 1024;

QString makeToken() {
    QByteArray raw(24, Qt::Uninitialized);
    auto* gen = QRandomGenerator::system();
    for (int i = 0; i < raw.size(); i += 4) {
        const quint32 r = gen->generate();
        for (int j = 0; j < 4 && i + j < raw.size(); ++j) {
            raw[i + j] = static_cast<char>((r >> (j * 8)) & 0xff);
        }
    }
    return QString::fromLatin1(fc::util::base64UrlEncode(raw));
}

// Build a full HTTP/1.1 response. Headers + body assembled in one buffer
// so we hand it to the socket as a single chunk (avoids partial-flush
// confusion if the kernel send buffer is small).
QByteArray buildResponse(int status, const char* phrase,
                         const QByteArray& body, const QByteArray& contentType,
                         const QByteArray& extraHeaders = {}) {
    QByteArray out;
    out.reserve(body.size() + 512);
    out.append("HTTP/1.1 ");
    out.append(QByteArray::number(status));
    out.append(' ');
    out.append(phrase);
    out.append("\r\n");
    out.append("Content-Type: ");
    out.append(contentType);
    out.append("\r\n");
    out.append("Content-Length: ");
    out.append(QByteArray::number(body.size()));
    out.append("\r\n");
    out.append("Connection: close\r\n");
    out.append("Cache-Control: no-store\r\n");
    out.append("X-Content-Type-Options: nosniff\r\n");
    out.append(extraHeaders);
    out.append("\r\n");
    out.append(body);
    return out;
}

// Synchronously push the full response onto the socket and start a clean
// close. We waitForBytesWritten so the kernel has a chance to ship the
// payload before we disconnectFromHost — without it, a quick
// disconnectFromHost + later object teardown can truncate the response.
void sendAndClose(QTcpSocket* sock, const QByteArray& response,
                  int status, const char* phrase) {
    const qint64 toWrite = response.size();
    const qint64 wrote = sock->write(response);
    sock->flush();
    sock->waitForBytesWritten(2000);
    qInfo("LocalHtmlServer: -> %d %s (%lld of %lld bytes flushed)",
          status, phrase, wrote, toWrite);
    sock->disconnectFromHost();
}

}  // namespace

LocalHtmlServer::LocalHtmlServer(const QByteArray& html,
                                  bool allowRemoteImages,
                                  QObject* parent)
    : QObject(parent),
      html_(html),
      token_(makeToken()),
      allowRemoteImages_(allowRemoteImages) {
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection,
            this,    &LocalHtmlServer::onNewConnection);

    lifetime_ = new QTimer(this);
    lifetime_->setSingleShot(true);
    lifetime_->setInterval(kIdleTimeoutMs);
    connect(lifetime_, &QTimer::timeout, this, [this] {
        qInfo("LocalHtmlServer: idle timeout (%d ms) — self-deleting",
              kIdleTimeoutMs);
        emit expired();
        deleteLater();
    });
}

bool LocalHtmlServer::start() {
    if (!server_->listen(QHostAddress::LocalHost, 0)) {
        qWarning("LocalHtmlServer: bind failed: %s",
                 qUtf8Printable(server_->errorString()));
        return false;
    }
    lifetime_->start();
    qInfo("LocalHtmlServer: listening on %s, %lld-byte body",
          qUtf8Printable(url().toString()), qint64(html_.size()));
    return true;
}

QUrl LocalHtmlServer::url() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/%2")
                    .arg(server_->serverPort())
                    .arg(token_));
}

void LocalHtmlServer::onNewConnection() {
    while (auto* sock = server_->nextPendingConnection()) {
        qInfo("LocalHtmlServer: connection from %s:%d",
              qUtf8Printable(sock->peerAddress().toString()),
              sock->peerPort());

        // Per-socket buffer + state. We can't use waitForReadyRead in an
        // event-loop slot without stalling the UI; instead we accumulate on
        // readyRead and dispatch as soon as the request line + blank line
        // delimiter is in the buffer.
        auto buffer = std::make_shared<QByteArray>();

        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);

        // Hard cap on read size + a per-socket watchdog. If the client
        // never sends a complete request, drop after kPerSocketReadMs.
        auto* watchdog = new QTimer(sock);
        watchdog->setSingleShot(true);
        watchdog->setInterval(kPerSocketReadMs);
        connect(watchdog, &QTimer::timeout, sock, [sock] {
            qWarning("LocalHtmlServer: socket read timed out — closing");
            sock->disconnectFromHost();
        });
        watchdog->start();

        connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buffer, watchdog] {
            buffer->append(sock->readAll());
            if (buffer->size() > kMaxRequestBytes) {
                qWarning("LocalHtmlServer: request exceeded %d bytes — closing",
                         kMaxRequestBytes);
                sock->disconnectFromHost();
                return;
            }
            // Wait until we've seen the request-line + blank line delimiter.
            const int hdrEnd = buffer->indexOf("\r\n\r\n");
            if (hdrEnd < 0) return;   // need more bytes

            watchdog->stop();
            const int lineEnd = buffer->indexOf("\r\n");
            const QByteArray firstLine = buffer->left(lineEnd);
            qInfo("LocalHtmlServer: <- %s",
                  qUtf8Printable(QString::fromLatin1(firstLine)));

            if (!firstLine.startsWith("GET ")) {
                sendAndClose(sock,
                    buildResponse(405, "Method Not Allowed",
                                  "Method not allowed.\n",
                                  "text/plain; charset=utf-8"),
                    405, "Method Not Allowed");
                return;
            }

            const QByteArray expected = "GET /" + token_.toLatin1();
            if (!firstLine.startsWith(expected)) {
                sendAndClose(sock,
                    buildResponse(404, "Not Found",
                                  "Not found.\n",
                                  "text/plain; charset=utf-8"),
                    404, "Not Found");
                return;
            }

            // CSP: scripts / iframes / forms / beacons stay locked down
            // regardless. The only thing allowRemoteImages_ widens is
            // img-src — equivalent to Gmail web's "Show images" toggle.
            const char* imgSrc = allowRemoteImages_
                ? "data: blob: https: http:"
                : "data: blob:";
            QByteArray extra;
            extra.append("Content-Security-Policy: default-src 'none'; ");
            extra.append("img-src ");
            extra.append(imgSrc);
            extra.append("; ");
            extra.append("style-src 'unsafe-inline'; "
                         "font-src data:; "
                         "base-uri 'none'; "
                         "form-action 'none'; "
                         "frame-ancestors 'none'\r\n");
            extra.append("Referrer-Policy: no-referrer\r\n");
            sendAndClose(sock,
                buildResponse(200, "OK", html_,
                              "text/html; charset=utf-8", extra),
                200, "OK");

            // Don't close the listening socket after the first 200 — Chromium
            // opens 4-5 parallel connections per page (favicon, sub-resource
            // pre-connects, IPv4-vs-IPv6 races) and any of them landing on a
            // closed server triggers an ERR_CONNECTION_REFUSED that keeps the
            // page in "Loading…" forever. Just emit the served() signal and
            // let lifetime_ tick down naturally — every subsequent request
            // for the same /<token> path serves the same HTML idempotently;
            // anything else (favicon, etc.) gets a clean 404.
            if (!firstServed_) {
                firstServed_ = true;
                emit served();
            }
        });
    }
}

}  // namespace fc::ui
