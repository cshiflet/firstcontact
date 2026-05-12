#include "RestClient.h"

#include "RateLimiter.h"
#include "SessionTransfer.h"
#include "auth/OAuthClient.h"

#include <QByteArray>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>

namespace fc::api {

namespace {

ApiErrorKind classify(int status, const QString& reason) {
    if (status == 401) return ApiErrorKind::Auth;
    if (status == 403 && (reason == QLatin1String("rateLimitExceeded") ||
                          reason == QLatin1String("userRateLimitExceeded"))) {
        return ApiErrorKind::RateLimited;
    }
    if (status == 404) return ApiErrorKind::NotFound;
    if (status == 429) return ApiErrorKind::RateLimited;
    if (status >= 500) return ApiErrorKind::Server;
    if (status >= 400) return ApiErrorKind::BadRequest;
    return ApiErrorKind::None;
}

ApiError parseErrorBody(int status, const QByteArray& body) {
    ApiError e;
    e.httpStatus = status;
    const auto doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const auto err = doc.object().value(QStringLiteral("error")).toObject();
        e.message = err.value(QStringLiteral("message")).toString();
        const auto errors = err.value(QStringLiteral("errors")).toArray();
        if (!errors.isEmpty()) {
            e.gmailReason = errors.first().toObject()
                              .value(QStringLiteral("reason")).toString();
        }
    }
    e.kind = classify(status, e.gmailReason);
    return e;
}

}  // namespace

RestClient::RestClient(fc::auth::OAuthClient* auth, QObject* parent)
    : QObject(parent),
      getToken_([auth]() { return auth->accessTokenBlocking(); }),
      nam_(new QNetworkAccessManager(this)) {}

RestClient::RestClient(TokenGetter tokenGetter, QObject* parent)
    : QObject(parent),
      getToken_(std::move(tokenGetter)),
      nam_(new QNetworkAccessManager(this)) {}

void RestClient::send(Verb verb, const QUrl& url, const QByteArray& body,
                      const QByteArray& contentType, DoneCb cb) {
    sendOnce(verb, url, body, contentType, /*attempt=*/1, /*refreshedOnce=*/false,
             std::move(cb));
}

void RestClient::sendOnce(Verb verb, const QUrl& url, const QByteArray& body,
                          const QByteArray& contentType,
                          int attempt, bool refreshedOnce, DoneCb cb) {
    const QString token = getToken_();
    if (token.isEmpty()) {
        cb({}, ApiError{ApiErrorKind::Auth, 0, QStringLiteral("no token"), {}});
        return;
    }

    QNetworkRequest req(url);
    req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    req.setRawHeader("Accept", "application/json");
    if (!contentType.isEmpty()) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    }
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = nullptr;
    switch (verb) {
        case Verb::Get:    reply = nam_->get(req); break;
        case Verb::Post:   reply = nam_->post(req, body); break;
        case Verb::Put:    reply = nam_->put(req, body); break;
        case Verb::Patch:  reply = nam_->sendCustomRequest(req, "PATCH", body); break;
        case Verb::Delete: reply = nam_->deleteResource(req); break;
    }

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, verb, url, body, contentType, attempt, refreshedOnce,
             cb = std::move(cb)]() mutable {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        const QNetworkReply::NetworkError nerr = reply->error();
        const int retryAfter = reply->rawHeader("Retry-After").toInt();
        reply->deleteLater();

        // Record the wire transfer for the status-bar bandwidth meter.
        // Counts every response (including errors) so retried requests
        // accumulate honestly. Header overhead is approximated inside
        // SessionTransfer::record.
        SessionTransfer::instance().record(data.size(), body.size());

        // Idempotent verbs are safe to retry blindly — the server
        // can absorb duplicates without observable side effects.
        // POST / PATCH / DELETE are NOT — retrying a transport-
        // failed POST /messages/send could double-send mail (the
        // server may have processed the request fully, just lost
        // the response). For those we retry ONLY on explicit 429
        // (server said "didn't process, try again") — never on 5xx
        // where the outcome is ambiguous.
        //
        // Caveat: Qt 6.4 QNAM does its own transparent retry below
        // this layer when the TCP connection drops before any HTTP
        // response bytes. That retry is method-agnostic and we
        // never see the original failure. The guard here still
        // protects against the more common 5xx case (where the
        // server DID write bytes, so QNAM does not auto-retry).
        // See test_rest_client.cpp::qnamTransparentlyRetries…
        // for the documenting test.
        const bool idempotent = (verb == Verb::Get || verb == Verb::Put);

        if (nerr != QNetworkReply::NoError && status == 0) {
            // Transport-level failure (no HTTP response).
            if (!idempotent) {
                cb({}, ApiError{ApiErrorKind::Network, 0,
                                QStringLiteral("network error (not retried — non-idempotent verb)"), {}});
                return;
            }
            const qint64 delay = RateLimiter::nextDelayMs(attempt);
            if (delay > 0) {
                QTimer::singleShot(int(delay), this,
                    [this, verb, url, body, contentType, attempt, refreshedOnce, cb]() mutable {
                        sendOnce(verb, url, body, contentType, attempt + 1, refreshedOnce, std::move(cb));
                    });
                return;
            }
            cb({}, ApiError{ApiErrorKind::Network, 0,
                            QStringLiteral("network error"), {}});
            return;
        }

        if (status >= 200 && status < 300) {
            cb(data, ApiError{});
            return;
        }

        ApiError err = parseErrorBody(status, data);

        if (err.kind == ApiErrorKind::Auth && !refreshedOnce) {
            // Try once more after a forced refresh; accessTokenBlocking will
            // observe the expiry window and refresh.
            sendOnce(verb, url, body, contentType, attempt, /*refreshedOnce=*/true,
                     std::move(cb));
            return;
        }

        const bool retryableServerStatus =
            err.kind == ApiErrorKind::RateLimited
            || (err.kind == ApiErrorKind::Server && idempotent);
        if (retryableServerStatus) {
            const qint64 delay = RateLimiter::nextDelayMs(attempt, retryAfter);
            if (delay > 0) {
                QTimer::singleShot(int(delay), this,
                    [this, verb, url, body, contentType, attempt, refreshedOnce, cb]() mutable {
                        sendOnce(verb, url, body, contentType, attempt + 1, refreshedOnce, std::move(cb));
                    });
                return;
            }
        }

        cb(data, err);
    });
}

// ---- Batch endpoint -------------------------------------------------------
//
// Gmail's /batch endpoint accepts a single multipart/mixed POST containing
// one application/http sub-part per logical request, and replies with a
// multipart/mixed body of the same shape on the response side. Each
// sub-request gets routed by the server as if it had arrived on its own
// connection, with one notable saving: the batch counts as a single
// concurrent request against the per-user quota regardless of how many
// sub-requests it carries. For our meta-only first-pass over freshly-
// listed message ids this turns ~50 round-trips into ~1.

namespace {

QByteArray verbName(RestClient::Verb v) {
    switch (v) {
        case RestClient::Verb::Get:    return "GET";
        case RestClient::Verb::Post:   return "POST";
        case RestClient::Verb::Put:    return "PUT";
        case RestClient::Verb::Patch:  return "PATCH";
        case RestClient::Verb::Delete: return "DELETE";
    }
    return "GET";
}

QByteArray randomBoundary() {
    // Boundary just needs to be unique enough that no message body can
    // accidentally contain it. Random 64-bit hex matches the convention
    // used by Google's own client libraries.
    const quint64 r1 = QRandomGenerator::global()->generate64();
    const quint64 r2 = QRandomGenerator::global()->generate64();
    return QByteArrayLiteral("batch_")
         + QByteArray::number(r1, 16)
         + QByteArray::number(r2, 16);
}

}  // namespace

void RestClient::sendBatch(std::vector<BatchSubRequest> requests,
                            BatchDoneCb cb) {
    if (requests.empty()) {
        cb({}, ApiError{});
        return;
    }

    const QByteArray boundary = randomBoundary();
    QByteArray body;
    body.reserve(int(requests.size()) * 256);
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& r = requests[i];
        body += "--";
        body += boundary;
        body += "\r\n";
        body += "Content-Type: application/http\r\n";
        // Content-ID is what Gmail echoes back on the response so we
        // can map a response part to its index. Using the form
        // <item-N> matches what the official client uses; the server
        // returns <response-item-N> in the corresponding response
        // part — that's what the parser keys on.
        body += "Content-ID: <item-";
        body += QByteArray::number(qulonglong(i));
        body += ">\r\n\r\n";
        body += verbName(r.verb);
        body += ' ';
        body += r.path;
        body += " HTTP/1.1\r\n";
        if (!r.body.isEmpty()) {
            if (!r.contentType.isEmpty()) {
                body += "Content-Type: ";
                body += r.contentType;
                body += "\r\n";
            }
            body += "Content-Length: ";
            body += QByteArray::number(r.body.size());
            body += "\r\n\r\n";
            body += r.body;
            body += "\r\n";
        } else {
            body += "\r\n";
        }
    }
    body += "--";
    body += boundary;
    body += "--\r\n";

    const QByteArray outerContentType =
        QByteArrayLiteral("multipart/mixed; boundary=") + boundary;

    const int expected = int(requests.size());
    const QUrl url(QStringLiteral("https://gmail.googleapis.com/batch"));

    // The outer call is a POST through `send`, so the existing
    // retry-only-on-429 policy applies (correct: a successful POST
    // could double-process sub-requests if we naively re-sent on a
    // 5xx). Sub-request-level retry is the caller's responsibility
    // — typical pattern is to re-batch failures with backoff.
    send(Verb::Post, url, body, outerContentType,
        [cb = std::move(cb), expected]
        (QByteArray respBody, ApiError err) {
            if (err) { cb({}, err); return; }
            // Need the outer Content-Type to find the response
            // boundary. send() doesn't surface headers, so we use a
            // tolerant scan: pull the boundary from the first part
            // marker in the body itself. Gmail's actual responses
            // begin with a CRLF before "--<boundary>", so we skip
            // any leading whitespace before locating the marker.
            // (The original tight check `startsWith("--")` rejected
            // valid responses because it didn't tolerate the
            // leading CRLF.)
            //
            // Distinct from the REQUEST boundary captured in the
            // outer scope: Gmail picks its own boundary for the
            // response. Renamed so -Wshadow stays clean and so a
            // reader doesn't mistake the two values for the same
            // string.
            QByteArray responseBoundary;
            int scan = 0;
            while (scan < respBody.size()
                   && (respBody[scan] == '\r' || respBody[scan] == '\n'
                       || respBody[scan] == ' '  || respBody[scan] == '\t')) {
                ++scan;
            }
            if (scan + 2 < respBody.size()
                && respBody[scan] == '-' && respBody[scan + 1] == '-') {
                const int markerStart = scan + 2;
                const int nlAfter = respBody.indexOf("\r\n", markerStart);
                if (nlAfter > markerStart) {
                    responseBoundary =
                        respBody.mid(markerStart, nlAfter - markerStart);
                }
            }
            if (responseBoundary.isEmpty()) {
                // Helpful diagnostic: include a short preview of what
                // Gmail actually returned. Common cause is the server
                // sending a JSON error envelope (e.g. 400/401) instead
                // of a multipart body, which the body-starts-with-"--"
                // check rejects.
                QByteArray preview = respBody.left(200);
                preview.replace('\n', "\\n").replace('\r', "\\r");
                cb({}, ApiError{ApiErrorKind::Parse, 0,
                                QStringLiteral("batch: no boundary in response "
                                                "(first 200B: %1)")
                                    .arg(QString::fromUtf8(preview)),
                                {}});
                return;
            }
            cb(RestClient::parseBatchResponse(respBody, responseBoundary,
                                                expected),
                ApiError{});
        });
}

std::vector<RestClient::BatchSubResult>
RestClient::parseBatchResponse(const QByteArray& body,
                                const QByteArray& boundary,
                                int expected) {
    std::vector<BatchSubResult> results(expected);

    const QByteArray sep = QByteArrayLiteral("--") + boundary;
    int searchFrom = 0;
    QRegularExpression contentIdRe(
        QStringLiteral("Content-ID:\\s*<response-item-(\\d+)>"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpression statusLineRe(
        QStringLiteral("^HTTP/[\\d.]+\\s+(\\d+)"));

    while (true) {
        const int start = body.indexOf(sep, searchFrom);
        if (start < 0) break;
        int afterSep = start + sep.size();
        // Trailing "--" marks the end of the multipart envelope.
        if (afterSep + 2 <= body.size()
            && body.at(afterSep) == '-' && body.at(afterSep + 1) == '-') {
            break;
        }
        // Skip the CRLF that follows the boundary line.
        if (afterSep + 1 < body.size()
            && body.at(afterSep) == '\r' && body.at(afterSep + 1) == '\n') {
            afterSep += 2;
        } else if (afterSep < body.size() && body.at(afterSep) == '\n') {
            afterSep += 1;
        }
        const int nextSep = body.indexOf(sep, afterSep);
        const int partEnd = (nextSep < 0) ? body.size() : nextSep;
        // Trim the single trailing CRLF that sits between the part and
        // the next boundary marker (RFC 2046: the CRLF preceding the
        // boundary delimiter is considered part of the delimiter, not
        // the part). Do NOT trim further — an HTTP response with no
        // body still terminates its headers with \r\n\r\n, and over-
        // eager trimming would erase that separator and break the
        // inner header/body split below.
        int partStop = partEnd;
        if (partStop - afterSep >= 2
            && body.at(partStop - 2) == '\r'
            && body.at(partStop - 1) == '\n') {
            partStop -= 2;
        } else if (partStop - afterSep >= 1
                   && body.at(partStop - 1) == '\n') {
            partStop -= 1;
        }
        const QByteArray part = body.mid(afterSep, partStop - afterSep);
        searchFrom = (nextSep < 0) ? body.size() : nextSep;

        // Split the part into outer headers vs the embedded HTTP
        // response. The outer headers carry Content-ID identifying
        // the index; the embedded response carries the inner status
        // line + headers + body.
        const int outerHdrEnd = part.indexOf("\r\n\r\n");
        if (outerHdrEnd < 0) continue;
        const QByteArray outerHeaders = part.left(outerHdrEnd);
        const QByteArray inner = part.mid(outerHdrEnd + 4);

        int idx = -1;
        const auto m = contentIdRe.match(QString::fromLatin1(outerHeaders));
        if (m.hasMatch()) idx = m.captured(1).toInt();
        if (idx < 0 || idx >= expected) continue;

        // Inner: status line, then inner headers, then body.
        const int innerHdrEnd = inner.indexOf("\r\n\r\n");
        if (innerHdrEnd < 0) continue;
        const QByteArray innerHead = inner.left(innerHdrEnd);
        const QByteArray innerBody = inner.mid(innerHdrEnd + 4);

        // First line of innerHead carries the status.
        int firstLineEnd = innerHead.indexOf("\r\n");
        if (firstLineEnd < 0) firstLineEnd = innerHead.size();
        const QByteArray statusLine = innerHead.left(firstLineEnd);
        const auto sm = statusLineRe.match(QString::fromLatin1(statusLine));
        int status = 0;
        if (sm.hasMatch()) status = sm.captured(1).toInt();

        results[idx].status = status;
        results[idx].body   = innerBody;
    }
    return results;
}

}  // namespace fc::api
