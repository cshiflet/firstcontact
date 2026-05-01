#pragma once

#include <QObject>

namespace fc::auth { class ClientConfig; class TokenStore; class OAuthClient; }
namespace fc::api  { class RestClient;   class GmailClient; }
namespace fc::sync { class SyncService;  class OutboxWorker; }
namespace fc::ui   { class MainWindow; }

namespace fc::app {

// Owns the long-lived service singletons and the main window. Created once
// from main() and torn down when QApplication exits.
class Bootstrap : public QObject {
    Q_OBJECT
public:
    explicit Bootstrap(QObject* parent = nullptr);
    ~Bootstrap() override;

    fc::ui::MainWindow* mainWindow() const;

private:
    fc::auth::ClientConfig*  config_;
    fc::auth::TokenStore*    tokenStore_;
    fc::auth::OAuthClient*   auth_;
    fc::api::RestClient*     rest_;
    fc::api::GmailClient*    gmail_;
    fc::sync::SyncService*   sync_;
    fc::sync::OutboxWorker*  outbox_;
    fc::ui::MainWindow*      window_;
};

}  // namespace fc::app
