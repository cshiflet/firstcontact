#include "Bootstrap.h"

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
    auth_       = new fc::auth::OAuthClient(config_, tokenStore_, this);
    rest_       = new fc::api::RestClient(auth_, this);
    gmail_      = new fc::api::GmailClient(rest_, this);
    sync_       = new fc::sync::SyncService(gmail_, this);
    outbox_     = new fc::sync::OutboxWorker(gmail_, this);
    pending_    = new fc::sync::PendingOpsWorker(gmail_, this);
    drafts_     = new fc::sync::DraftSync(gmail_, this);
    window_     = new fc::ui::MainWindow(config_, auth_, gmail_,
                                         sync_, outbox_, pending_, drafts_);
}

Bootstrap::~Bootstrap() {
    delete window_;
    delete config_;
}

fc::ui::MainWindow* Bootstrap::mainWindow() const { return window_; }

}  // namespace fc::app
