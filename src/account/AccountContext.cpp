#include "AccountContext.h"

#include "api/GmailClient.h"
#include "api/RestClient.h"
#include "auth/ClientConfig.h"
#include "auth/OAuthClient.h"
#include "auth/TokenStore.h"
#include "sync/SyncService.h"

namespace fc::account {

AccountContext::AccountContext(const QString& accountId,
                                fc::auth::ClientConfig* config,
                                fc::auth::TokenStore*   tokenStore,
                                QObject* parent)
    : QObject(parent), accountId_(accountId) {
    // Plain parented-QObject hierarchy on the UI thread. Deleting
    // the AccountContext cascades destruction down through sync →
    // gmail → rest → auth via Qt's standard child cleanup.
    auth_  = new fc::auth::OAuthClient(config, tokenStore, accountId, this);
    rest_  = new fc::api::RestClient(auth_, this);
    gmail_ = new fc::api::GmailClient(rest_, this);
    sync_  = new fc::sync::SyncService(gmail_, this);
    sync_->setAccountId(accountId);
    qInfo("AccountContext: built ctx=%p sync=%p for accountId='%s'",
          static_cast<void*>(this), static_cast<void*>(sync_),
          qUtf8Printable(accountId));
}

AccountContext::~AccountContext() = default;

}  // namespace fc::account
