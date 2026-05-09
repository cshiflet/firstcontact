#pragma once

#include "Errors.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

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

private:
    TokenGetter             getToken_;
    QNetworkAccessManager*  nam_;

    void sendOnce(Verb verb, const QUrl& url, const QByteArray& body,
                  const QByteArray& contentType,
                  int attempt, bool refreshedOnce, DoneCb cb);
};

}  // namespace fc::api
