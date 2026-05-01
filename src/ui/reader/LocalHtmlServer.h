#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace fc::ui {

// One-shot loopback HTTP server that serves a single sanitized HTML message
// to the user's system browser, then shuts down.
//
// Used by the "Open in browser" HTML preview mode so we can let the OS
// browser render rich email content (full fidelity, accessibility, find,
// zoom) without dragging Qt WebEngine into the process.
//
// Design choices:
//   - Binds 127.0.0.1 only on an ephemeral port; never reachable from the
//     network.
//   - The URL contains a 24-byte random token; only requests whose path
//     matches /<token> are served. Anyone else gets a 404. Defends against
//     other local processes scanning loopback ports.
//   - Serves at most one successful response, then closes the listening
//     socket and self-deletes after a brief flush delay.
//   - Idle lifetime cap (60 s by default): the server self-deletes if no
//     valid request ever arrives.
//   - Response carries a strict Content-Security-Policy that blocks every
//     remote load; only inline styles, data: images, and data: fonts are
//     allowed. Combined with HtmlSanitizer's tag/attribute whitelist this
//     means the served page can't beacon, run JS, or load tracking pixels
//     even though the OS browser is doing the rendering.
class LocalHtmlServer : public QObject {
    Q_OBJECT
public:
    explicit LocalHtmlServer(const QByteArray& html, QObject* parent = nullptr);

    // Binds the loopback socket. Returns false on bind failure.
    bool start();

    // The URL the user's browser should open. Valid only after start()
    // returns true.
    QUrl url() const;

signals:
    void served();
    void expired();

private slots:
    void onNewConnection();

private:
    QTcpServer* server_   = nullptr;
    QByteArray  html_;
    QString     token_;
    QTimer*     lifetime_ = nullptr;
};

}  // namespace fc::ui
