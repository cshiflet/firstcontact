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
    // Each context owns its full per-account stack. QObject parenting
    // cascades destruction on sign-out (delete the context).
    auth_  = new fc::auth::OAuthClient(config, tokenStore, this);
    rest_  = new fc::api::RestClient(auth_, this);
    gmail_ = new fc::api::GmailClient(rest_, this);
    sync_  = new fc::sync::SyncService(gmail_, this);
    sync_->setAccountId(accountId);
}

AccountContext::~AccountContext() = default;

}  // namespace fc::account
