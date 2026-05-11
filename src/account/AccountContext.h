#pragma once

#include <QObject>
#include <QString>

class QThread;

namespace fc::auth { class OAuthClient; class TokenStore; class ClientConfig; }
namespace fc::api  { class RestClient; class GmailClient; }
namespace fc::sync { class SyncService; }

namespace fc::account {

// Per-account API + sync stack. Owned by AccountManager.
//
// Ownership: AccountManager holds a QHash<QString, AccountContext*>
// keyed by account id. Each context owns a dedicated QThread on
// which its OAuthClient / RestClient / GmailClient / SyncService all
// live — heavy SQL batch upserts and Gmail REST traffic used to
// block the UI thread during top-up; moving the stack off-UI keeps
// the event loop responsive while a sync ticks.
//
// Cross-thread access rules:
//   - `accountId()` / `degraded()` are pure-UI-thread state and stay
//     readable from the UI thread without synchronisation.
//   - `auth()` / `rest()` / `gmail()` / `sync()` return pointers to
//     QObjects that live on the sync thread. Any non-thread-safe
//     method on them MUST be invoked via QMetaObject::invokeMethod
//     with Qt::QueuedConnection. Signals emitted by those objects
//     reach UI-thread slots via auto-queued connections.
//   - The only methods callable directly from any thread are those
//     documented as thread-safe in their headers (e.g.
//     OAuthClient::isAuthorized / accountEmail / accessTokenBlocking,
//     all of which take an internal mutex).
class AccountContext : public QObject {
    Q_OBJECT
public:
    AccountContext(const QString& accountId,
                   fc::auth::ClientConfig* config,
                   fc::auth::TokenStore*   tokenStore,
                   QObject* parent = nullptr);
    ~AccountContext() override;

    QString accountId() const { return accountId_; }
    fc::auth::OAuthClient*  auth()  const { return auth_; }
    fc::api::RestClient*    rest()  const { return rest_; }
    fc::api::GmailClient*   gmail() const { return gmail_; }
    fc::sync::SyncService*  sync()  const { return sync_; }

    // The sync thread the per-account stack lives on. Exposed so
    // callers that need to QMetaObject::invokeMethod into the stack
    // can target the right thread when the receiver itself isn't
    // convenient. nullptr only between ctor end and thread start
    // (which is synchronous on construction, so in practice never
    // observed from outside this file).
    QThread* syncThread() const { return syncThread_; }

    // The account is fully signed in (refresh token usable). Set
    // false when refresh fails so the UI can surface a re-sign-in
    // affordance per account.
    bool degraded() const { return degraded_; }
    void setDegraded(bool d) { degraded_ = d; }

private:
    QString accountId_;
    QThread*                syncThread_ = nullptr;
    fc::auth::OAuthClient*  auth_  = nullptr;
    fc::api::RestClient*    rest_  = nullptr;
    fc::api::GmailClient*   gmail_ = nullptr;
    fc::sync::SyncService*  sync_  = nullptr;
    bool                    degraded_ = false;
};

}  // namespace fc::account
