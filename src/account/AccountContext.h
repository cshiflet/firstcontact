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
// keyed by account id. Each context QObject-parents its members so
// destruction is automatic when the context is deleted (sign-out).
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
