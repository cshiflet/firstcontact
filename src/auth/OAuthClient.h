#pragma once

#include "TokenStore.h"

#include <QObject>
#include <QString>

class QOAuth2AuthorizationCodeFlow;
class QOAuthHttpServerReplyHandler;

namespace fc::auth {

class ClientConfig;

// Owns the OAuth 2.0 + PKCE (S256) flow against Google's endpoints, plus
// silent refresh and token persistence via TokenStore.
//
// Lifetime: created on the main thread (loopback browser flow needs the GUI
// event loop). Token getters are guarded by a mutex so the sync thread can
// pull a valid Bearer header.
class OAuthClient : public QObject {
    Q_OBJECT
public:
    OAuthClient(ClientConfig* config, TokenStore* store, QObject* parent = nullptr);
    ~OAuthClient() override;

    bool isAuthorized() const;
    QString accountEmail() const;

    // Returns a non-empty access token, refreshing if within 60s of expiry.
    // Blocking call safe to use from the sync thread.
    QString accessTokenBlocking();

    // Triggers the browser-based authorization flow. Emits granted/failed.
    void authorize();

    // Revokes the refresh token at Google and clears the keychain entry.
    void signOut();

signals:
    void granted();
    void failed(const QString& reason);
    void signedOut();

private:
    void hydrateFromStore();
    void wireFlow();
    void persistFromFlow();
    bool refreshIfNeededLocked();

    struct Impl;
    Impl* d_;
};

}  // namespace fc::auth
