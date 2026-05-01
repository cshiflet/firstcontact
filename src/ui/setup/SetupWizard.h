#pragma once

#include <QDialog>

class QLineEdit;
class QLabel;

namespace fc::auth { class ClientConfig; }

namespace fc::ui {

// First-run dialog asking the user to paste a Google Cloud OAuth Desktop-app
// client_id. Includes step-by-step instructions and a link out to the Google
// Cloud Console. No client secret needed (PKCE).
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
    QLabel*    statusLabel_;
};

}  // namespace fc::ui
