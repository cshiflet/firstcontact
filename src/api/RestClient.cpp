#include "RestClient.h"

#include "RateLimiter.h"
#include "SessionTransfer.h"
#include "auth/OAuthClient.h"

#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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
    : QObject(parent), auth_(auth), nam_(new QNetworkAccessManager(this)) {}

void RestClient::send(Verb verb, const QUrl& url, const QByteArray& body,
                      const QByteArray& contentType, DoneCb cb) {
    sendOnce(verb, url, body, contentType, /*attempt=*/1, /*refreshedOnce=*/false,
             std::move(cb));
}

void RestClient::sendOnce(Verb verb, const QUrl& url, const QByteArray& body,
                          const QByteArray& contentType,
                          int attempt, bool refreshedOnce, DoneCb cb) {
    const QString token = auth_->accessTokenBlocking();
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
        // (server said "didn't process, try again") — never on
        // transport or 5xx where the outcome is ambiguous.
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

}  // namespace fc::api
