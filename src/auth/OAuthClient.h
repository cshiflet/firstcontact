#pragma once

#include "TokenStore.h"

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

class QOAuthHttpServerReplyHandler;

namespace fc::auth {

class ClientConfig;

// Owns the OAuth 2.0 + PKCE (S256) flow against Google's endpoints, plus
// silent refresh and token persistence via TokenStore.
//
// Account binding: an OAuthClient is either bound to a specific account
// id (the AccountContext case — it hydrates from / persists to that
// keychain slot) or unbound (constructed with an empty accountId, used
// to drive a brand-new sign-in before an accounts row exists). An
// unbound client never persists; the caller is expected to mint the
// accounts row on grant + email arrival, then call bindAccountId() to
// flip the client into bound mode and write the tokens to the slot.
//
// Lifetime: created on the main thread (loopback browser flow needs the
// GUI event loop). Token getters are guarded by a mutex so the sync
// thread can pull a valid Bearer header.
class OAuthClient : public QObject {
    Q_OBJECT
public:
    OAuthClient(ClientConfig* config, TokenStore* store,
                QString accountId, QObject* parent = nullptr);
    ~OAuthClient() override;

    // Non-empty for bound clients. Empty for the not-yet-an-account
    // sign-in flow until bindAccountId() is called.
    QString accountId() const;

    // Switches an unbound client into the bound state and persists
    // whatever tokens are currently in hand to the new slot. No-op
    // (and a qWarning) if the client is already bound to a different
    // account, or if there are no tokens to persist.
    void bindAccountId(const QString& accountId);

    // Snapshot of the current tokens. Used by the Add-account flow to
    // copy tokens out of a transient unbound client into the bound
    // OAuthClient that lives on the freshly-created AccountContext —
    // see MainWindow::beginAddAccountFlow.
    TokenStore::Tokens tokensSnapshot() const;

    // Replace this (already-bound) client's in-memory tokens with the
    // ones produced by a transient sign-in flow, and persist to the
    // bound slot. Used by the Add-account path so the new
    // AccountContext can start serving authorized requests
    // synchronously, without waiting for an async keychain reload.
    void adoptTokens(const TokenStore::Tokens& tokens);

    bool isAuthorized() const;
    QString accountEmail() const;

    // Persists the account email after the first profile fetch. Called
    // by MainWindow once SyncService::profileFetched fires, so the
    // toolbar / account menu can show the signed-in identity.
    void setAccountEmail(const QString& email);

    // Returns a non-empty access token, refreshing if within 60s of expiry.
    // When forceRefresh is true, refreshes even if the current access token
    // has not expired yet; used after an HTTP 401 proves the server rejected
    // our cached token.
    // Blocking call safe to use from the sync thread.
    QString accessTokenBlocking(bool forceRefresh = false);

    // Triggers the browser-based authorization flow. Emits granted/failed.
    void authorize();

    // Revokes the refresh token at Google and clears the keychain entry.
    // No-op if the client is unbound.
    void signOut();

signals:
    void granted();
    void failed(const QString& reason);
    void signedOut();
    // Fires the first time tokens land (or fail to land) from the
    // keychain after construction. The hydrate is async — without this
    // signal, MainWindow's initial refreshAccountMenu runs before
    // isAuthorized() is meaningful and shows "Sign in" even when
    // the user actually has cached credentials.
    void tokensLoaded();

    // Emitted when the flow has prepared the authorization URL. Subscribers
    // (MainWindow) can display a copy-the-URL fallback if the system browser
    // failed to open automatically — covers AppImage / sandboxed environments
    // where QDesktopServices::openUrl can silently no-op.
    void browserAuthRequested(const QUrl& url, bool openedAutomatically);

private:
    void hydrateFromStore();
    void ensureHandler();
    void onAuthCodeCallback(const QVariantMap& params);
    void exchangeCodeForTokens(const QString& code);
    bool refreshIfNeededLocked(bool forceRefresh);

    struct Impl;
    Impl* d_;
};

}  // namespace fc::auth
