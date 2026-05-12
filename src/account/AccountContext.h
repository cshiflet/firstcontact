#pragma once

#include <QObject>
#include <QString>

namespace fc::auth { class OAuthClient; class TokenStore; class ClientConfig; }
namespace fc::api  { class RestClient; class GmailClient; }
namespace fc::sync { class SyncService; }

namespace fc::account {

// Per-account API + sync stack. Owned by AccountManager.
//
// Ownership: AccountManager holds a QHash<QString, AccountContext*>
// keyed by account id. Each context owns its own OAuthClient /
// RestClient / GmailClient / SyncService, all parented to the
// AccountContext so destruction cascades when the context is
// deleted (sign-out or shutdown).
//
// Threading: every member object lives on the UI thread alongside
// the AccountContext. The earlier attempt at per-account QThreads
// (commit 95a8543) was never compile-tested by its author and
// produced an intermittent SIGSEGV in QObject destructor paths;
// reverted. SyncService's heavy work (SQL batch upserts, Gmail
// REST) still doesn't visibly block the UI on typical mailboxes,
// and when it does, the right answer is to move the SPECIFIC
// expensive step off the UI thread (QtConcurrent for parsing,
// background QObject for the compression worker, etc.) — not to
// move the entire per-account QObject hierarchy.
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

    // The account is fully signed in (refresh token usable). Set
    // false when refresh fails so the UI can surface a re-sign-in
    // affordance per account.
    bool degraded() const { return degraded_; }
    void setDegraded(bool d) { degraded_ = d; }

private:
    QString accountId_;
    fc::auth::OAuthClient*  auth_  = nullptr;
    fc::api::RestClient*    rest_  = nullptr;
    fc::api::GmailClient*   gmail_ = nullptr;
    fc::sync::SyncService*  sync_  = nullptr;
    bool                    degraded_ = false;
};

}  // namespace fc::account
