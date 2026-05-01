#include "LocalHtmlServer.h"

#include "util/Base64Url.h"

#include <QHostAddress>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace fc::ui {

namespace {

constexpr int kIdleTimeoutMs = 60'000;

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

void writeAndClose(QTcpSocket* sock, int status, const char* phrase,
                   const QByteArray& body, const QByteArray& contentType,
                   const QByteArray& extraHeaders = {}) {
    QByteArray out;
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
    sock->write(out);
    sock->flush();
    sock->disconnectFromHost();
}

}  // namespace

LocalHtmlServer::LocalHtmlServer(const QByteArray& html, QObject* parent)
    : QObject(parent), html_(html), token_(makeToken()) {
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection,
            this,    &LocalHtmlServer::onNewConnection);

    lifetime_ = new QTimer(this);
    lifetime_->setSingleShot(true);
    lifetime_->setInterval(kIdleTimeoutMs);
    connect(lifetime_, &QTimer::timeout, this, [this] {
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
    return true;
}

QUrl LocalHtmlServer::url() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/%2")
                    .arg(server_->serverPort())
                    .arg(token_));
}

void LocalHtmlServer::onNewConnection() {
    auto* sock = server_->nextPendingConnection();
    if (!sock) return;
    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);

    // Read the request line + headers. Browsers issue the GET in a single
    // small packet; 2 s is plenty.
    if (!sock->waitForReadyRead(2000)) {
        sock->close();
        return;
    }

    const QByteArray req = sock->readAll();
    const int lineEnd = req.indexOf("\r\n");
    const QByteArray firstLine = (lineEnd > 0) ? req.left(lineEnd) : req;

    if (!firstLine.startsWith("GET ")) {
        writeAndClose(sock, 405, "Method Not Allowed",
                      "Method not allowed.\n", "text/plain; charset=utf-8");
        return;
    }

    // GET /<token>... HTTP/1.1
    const QByteArray expected = "GET /" + token_.toLatin1();
    if (!firstLine.startsWith(expected)) {
        writeAndClose(sock, 404, "Not Found",
                      "Not found.\n", "text/plain; charset=utf-8");
        return;
    }

    QByteArray headers;
    headers.append("Content-Security-Policy: default-src 'none'; "
                   "img-src data: blob:; "
                   "style-src 'unsafe-inline'; "
                   "font-src data:; "
                   "base-uri 'none'; "
                   "form-action 'none'; "
                   "frame-ancestors 'none'\r\n");
    headers.append("Referrer-Policy: no-referrer\r\n");
    writeAndClose(sock, 200, "OK", html_, "text/html; charset=utf-8", headers);

    // One-shot: stop accepting further connections, self-delete shortly so
    // the socket has time to flush and close cleanly.
    server_->close();
    lifetime_->stop();
    emit served();
    QTimer::singleShot(2000, this, &QObject::deleteLater);
}

}  // namespace fc::ui
