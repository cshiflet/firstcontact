#pragma once

#include <QDialog>

class QLineEdit;
class QLabel;

namespace fc::auth { class ClientConfig; }

namespace fc::ui {

// First-run dialog asking the user to paste a Google Cloud OAuth Desktop-app
// client_id and client_secret. Despite using PKCE, Google's token endpoint
// still requires the secret for Desktop clients ("isn't a true secret
// because it's bundled with the source code", per Google's docs).
class SetupWizard : public QDialog {
    Q_OBJECT
public:
    SetupWizard(fc::auth::ClientConfig* config, QWidget* parent = nullptr);

private slots:
    void onSave();
    void onOpenConsole();

private:
    fc::auth::ClientConfig* config_;
    QLineEdit* idEdit_;
    QLineEdit* secretEdit_;
    QLabel*    statusLabel_;
};

}  // namespace fc::ui
