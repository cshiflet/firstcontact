#pragma once

#include <QDialog>

class QVBoxLayout;

namespace fc::auth { class OAuthClient; }

namespace fc::ui {

// Single-screen account manager. v1 ships single-account, so the body
// shows at most one row; the layout is shaped for multi-account so the
// future "Add another" path lights up cleanly.
//
// MainWindow owns the actual sign-in / sign-out / worker-stop work —
// the dialog just announces intent via signals. Connecting from
// MainWindow lets us reuse onSignOut (which clears tokens, stops the
// sync workers, and clears the cached profile email) and onSignIn
// (which surfaces the SetupWizard if the OAuth client_id is missing,
// then runs the OAuth dance) without duplicating their state-machine
// glue.
class AccountManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit AccountManagerDialog(fc::auth::OAuthClient* auth,
                                   QWidget* parent = nullptr);

signals:
    void signOutRequested();
    void addAccountRequested();

private:
    void rebuild();

    fc::auth::OAuthClient* auth_;
    QVBoxLayout*           accountList_ = nullptr;
};

}  // namespace fc::ui
