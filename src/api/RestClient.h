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
//   - injects "Authorization: Bearer …" via OAuthClient::accessTokenBlocking,
//   - retries transient 401 once after token refresh,
//   - retries 429/5xx with jittered exponential backoff.
//
// Lives on the sync thread — its QNAM has thread affinity there.
class RestClient : public QObject {
    Q_OBJECT
public:
    using DoneCb = std::function<void(QByteArray body, ApiError err)>;

    enum class Verb { Get, Post, Put, Patch, Delete };

    RestClient(fc::auth::OAuthClient* auth, QObject* parent = nullptr);

    // Fire and forget; cb runs on this object's thread.
    void send(Verb verb,
              const QUrl& url,
              const QByteArray& body,
              const QByteArray& contentType,
              DoneCb cb);

private:
    fc::auth::OAuthClient*  auth_;
    QNetworkAccessManager*  nam_;

    void sendOnce(Verb verb, const QUrl& url, const QByteArray& body,
                  const QByteArray& contentType,
                  int attempt, bool refreshedOnce, DoneCb cb);
};

}  // namespace fc::api
