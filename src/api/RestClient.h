#pragma once

#include "Errors.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <vector>

class QNetworkAccessManager;

namespace fc::auth { class OAuthClient; }

namespace fc::api {

// Thin async HTTP layer over QNetworkAccessManager that:
//   - injects "Authorization: Bearer …" via the token getter,
//   - retries transient 401 once after token refresh,
//   - retries 429/5xx with jittered exponential backoff (idempotent verbs only).
//
// Lives on the sync thread — its QNAM has thread affinity there.
//
// The token getter is a callable that returns the current access token,
// blocking long enough to refresh if needed. In production the lambda
// dispatches to OAuthClient::accessTokenBlocking; tests inject a
// scriptable getter so the retry / refresh logic can be exercised
// without touching QtKeychain.
class RestClient : public QObject {
    Q_OBJECT
public:
    using DoneCb = std::function<void(QByteArray body, ApiError err)>;
    using TokenGetter = std::function<QString()>;

    enum class Verb { Get, Post, Put, Patch, Delete };

    // ---------- Batch endpoint --------------------------------------------
    //
    // One sub-request inside a Gmail /batch envelope. `path` is the path
    // under https://gmail.googleapis.com (including any query string),
    // e.g. "/gmail/v1/users/me/messages/abc?format=metadata". For GET
    // sub-requests, `body` is empty. Other verbs are accepted but only
    // GET is exercised in production today.
    struct BatchSubRequest {
        Verb       verb = Verb::Get;
        QByteArray path;
        QByteArray body;          // empty for GET
        QByteArray contentType;   // populated only when body is non-empty
    };

    // One parsed sub-response slot. `status` is the HTTP status from the
    // inner response (0 if the part couldn't be parsed at all). `body`
    // is the response payload (empty for parse failures). Results align
    // by index with the input requests vector.
    struct BatchSubResult {
        int        status = 0;
        QByteArray body;
    };

    using BatchDoneCb = std::function<void(std::vector<BatchSubResult> results,
                                             ApiError err)>;

    // Production constructor — adapts OAuthClient::accessTokenBlocking into
    // the getter form internally. Existing call sites unchanged.
    RestClient(fc::auth::OAuthClient* auth, QObject* parent = nullptr);

    // Test / DI constructor — caller supplies the token getter directly.
    RestClient(TokenGetter tokenGetter, QObject* parent = nullptr);

    // Fire and forget; cb runs on this object's thread.
    void send(Verb verb,
              const QUrl& url,
              const QByteArray& body,
              const QByteArray& contentType,
              DoneCb cb);

    // Sends one Gmail /batch round-trip. Builds a multipart/mixed body
    // from `requests`, POSTs to https://gmail.googleapis.com/batch, and
    // parses the multipart response into per-sub-request results in
    // the same order as `requests`.
    //
    // Retry policy: the outer POST retries only on 429 (same as any
    // other POST through `send`). Sub-request statuses are surfaced
    // unchanged for the caller to fan back to per-request retries if
    // it wants. A network-level failure (no usable HTTP response) is
    // reported via the top-level ApiError; per-result entries are
    // returned empty in that case.
    //
    // Static parser exposed for tests that build a synthetic batch
    // response by hand.
    void sendBatch(std::vector<BatchSubRequest> requests, BatchDoneCb cb);

    static std::vector<BatchSubResult>
    parseBatchResponse(const QByteArray& body,
                        const QByteArray& boundary,
                        int expected);

private:
    TokenGetter             getToken_;
    QNetworkAccessManager*  nam_;

    void sendOnce(Verb verb, const QUrl& url, const QByteArray& body,
                  const QByteArray& contentType,
                  int attempt, bool refreshedOnce, DoneCb cb);
};

}  // namespace fc::api
