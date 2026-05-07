#pragma once

#include <QDialog>
#include <QString>

class QVBoxLayout;

namespace fc::auth    { class OAuthClient; }
namespace fc::account { class AccountManager; }

namespace fc::ui {

// Multi-row account manager. Renders one row per signed-in account
// with a "Sign out" button + "Make default" radio per row, plus an
// "Add another account…" footer button.
//
// MainWindow owns the actual sign-in / sign-out / worker-stop work —
// the dialog just announces intent via signals. signOutRequested
// carries an accountId so MainWindow can pick the right context.
class AccountManagerDialog : public QDialog {
    Q_OBJECT
public:
    AccountManagerDialog(fc::auth::OAuthClient*       auth,
                         fc::account::AccountManager* accounts,
                         QWidget* parent = nullptr);

signals:
    void signOutRequested(const QString& accountId);
    void addAccountRequested();

private:
    void rebuild();

    fc::auth::OAuthClient*        auth_;
    fc::account::AccountManager*  accounts_;
    QVBoxLayout*                  accountList_ = nullptr;
};

}  // namespace fc::ui
