#include "Bootstrap.h"

#include "account/AccountContext.h"
#include "account/AccountManager.h"
#include "api/GmailClient.h"
#include "api/RestClient.h"
#include "auth/ClientConfig.h"
#include "auth/OAuthClient.h"
#include "auth/TokenStore.h"
#include "cache/Database.h"
#include "sync/DraftSync.h"
#include "sync/OutboxWorker.h"
#include "sync/PendingOpsWorker.h"
#include "sync/SyncService.h"
#include "ui/MainWindow.h"

namespace fc::app {

Bootstrap::Bootstrap(QObject* parent) : QObject(parent) {
    fc::cache::Database::initialize();

    config_     = new fc::auth::ClientConfig;            // value type, raw owner
    tokenStore_ = new fc::auth::TokenStore(this);

    // AccountManager owns the per-account stack (one OAuthClient /
    // RestClient / GmailClient / SyncService per signed-in account).
    // It builds an AccountContext for every accounts row at startup.
    accounts_   = new fc::account::AccountManager(config_, tokenStore_, this);

    // Legacy single-instance pointers. These were originally aliased
    // to the current account's context, but that turned every
    // MainWindow side-effect on `sync_` / `auth_` (e.g.
    // clearAccountUiState's `sync_->setAccountId({})`) into a
    // mutation of the active context's stack — which silently broke
    // sync at startup whenever an accounts row already existed.
    //
    // The aliases now always own a separate, never-bound "anonymous"
    // stack. Per-account work routes through accounts_->contextFor()
    // / accounts_->currentContext() everywhere that matters.
    auth_  = new fc::auth::OAuthClient(config_, tokenStore_,
                                        /*accountId=*/QString(), this);
    rest_  = new fc::api::RestClient(auth_, this);
    gmail_ = new fc::api::GmailClient(rest_, this);
    sync_  = new fc::sync::SyncService(gmail_, this);

    auto resolver = [mgr = accounts_](const QString& accountId)
                        -> fc::api::GmailClient* {
        if (auto* ctx = mgr->contextFor(accountId)) return ctx->gmail();
        return nullptr;
    };
    outbox_     = new fc::sync::OutboxWorker(resolver, this);
    pending_    = new fc::sync::PendingOpsWorker(resolver, this);
    drafts_     = new fc::sync::DraftSync(resolver, this);

    window_     = new fc::ui::MainWindow(config_, auth_, gmail_,
                                         sync_, outbox_, pending_, drafts_,
                                         accounts_);
}

Bootstrap::~Bootstrap() {
    delete window_;
    delete config_;
}

fc::ui::MainWindow* Bootstrap::mainWindow() const { return window_; }

}  // namespace fc::app
