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
//   - Idempotent: every request to /<token> serves the same HTML. Browsers
//     routinely fire 4-5 parallel connections per page (sub-resource pre-
//     connects, favicon, IPv4-vs-IPv6 races) and we don't want any of them
//     stranded on a closed socket — that's what makes the page hang in
//     "Loading…". The lifetime timer (5 minutes by default) handles
//     teardown so we don't leak the listening port forever.
//   - Response carries a strict Content-Security-Policy that blocks every
//     remote load; only inline styles, data: images, and data: fonts are
//     allowed. Combined with HtmlSanitizer's tag/attribute whitelist this
//     means the served page can't beacon, run JS, or load tracking pixels
//     even though the OS browser is doing the rendering.
class LocalHtmlServer : public QObject {
    Q_OBJECT
public:
    // imgSrcAdditions is appended verbatim to the served
    // Content-Security-Policy's `img-src` directive (which already
    // allows `data:` and `blob:`). Pass "" for the strictest mode
    // (no remote images at all); pass "https://wsrv.nl/" or similar
    // when the HTML has been pre-rewritten to route through a proxy
    // host the browser is allowed to contact. Scripts, iframes, fonts,
    // beacons, and forms stay blocked regardless.
    explicit LocalHtmlServer(const QByteArray& html,
                              const QString& imgSrcAdditions = QString(),
                              QObject* parent = nullptr);

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
    QTcpServer* server_            = nullptr;
    QByteArray  html_;
    QString     token_;
    QTimer*     lifetime_          = nullptr;
    bool        firstServed_       = false;
    QByteArray  imgSrcAdditions_;
};

}  // namespace fc::ui
