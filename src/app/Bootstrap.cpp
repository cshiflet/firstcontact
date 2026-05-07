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

    // The legacy single-instance pointers below now alias the current
    // account's context. MainWindow still consumes them directly until
    // step 8 reroutes through accounts_->currentContext(). On account
    // switch, MainWindow rebinds these via accounts_->currentContext().
    if (auto* ctx = accounts_->currentContext()) {
        auth_  = ctx->auth();
        rest_  = ctx->rest();
        gmail_ = ctx->gmail();
        sync_  = ctx->sync();
    } else {
        // No accounts row yet (fresh install pre-sign-in): build a
        // shared "anonymous" stack so the sign-in flow has somewhere
        // to land. The OAuthClient here loads no slot (Database
        // returns empty default). Once add() runs, the rebuilt
        // currentContext takes over.
        auth_  = new fc::auth::OAuthClient(config_, tokenStore_, this);
        rest_  = new fc::api::RestClient(auth_, this);
        gmail_ = new fc::api::GmailClient(rest_, this);
        sync_  = new fc::sync::SyncService(gmail_, this);
    }

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
