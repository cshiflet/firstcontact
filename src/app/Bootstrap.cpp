#include "Bootstrap.h"

#include "api/GmailClient.h"
#include "api/RestClient.h"
#include "auth/ClientConfig.h"
#include "auth/OAuthClient.h"
#include "auth/TokenStore.h"
#include "ui/MainWindow.h"

namespace fc::app {

Bootstrap::Bootstrap(QObject* parent) : QObject(parent) {
    config_     = new fc::auth::ClientConfig;            // value type, raw owner
    tokenStore_ = new fc::auth::TokenStore(this);
    auth_       = new fc::auth::OAuthClient(config_, tokenStore_, this);
    rest_       = new fc::api::RestClient(auth_, this);  // Phase-2 moves to sync thread
    gmail_      = new fc::api::GmailClient(rest_, this);
    window_     = new fc::ui::MainWindow(config_, auth_, gmail_);
}

Bootstrap::~Bootstrap() {
    delete window_;
    delete config_;
}

fc::ui::MainWindow* Bootstrap::mainWindow() const { return window_; }

}  // namespace fc::app
