#include "AccountManagerDialog.h"

#include "auth/OAuthClient.h"
#include "cache/Database.h"
#include "cache/MetaRepository.h"
#include "ui/common/IconLoader.h"

#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace fc::ui {

AccountManagerDialog::AccountManagerDialog(fc::auth::OAuthClient* auth,
                                            QWidget* parent)
    : QDialog(parent), auth_(auth) {
    setWindowTitle(tr("Manage accounts"));
    resize(520, 320);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(14);

    auto* title = new QLabel(
        tr("<h3 style='margin:0'>Accounts</h3>"), this);
    title->setTextFormat(Qt::RichText);
    root->addWidget(title);

    auto* hint = new QLabel(tr(
        "FirstContact v1 supports a single Google account. Sign out to "
        "remove the active account, or use <b>Add another account</b> to "
        "switch to a different one."), this);
    hint->setObjectName(QStringLiteral("FormHint"));
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::RichText);
    root->addWidget(hint);

    accountList_ = new QVBoxLayout;
    accountList_->setSpacing(8);
    root->addLayout(accountList_);
    rebuild();

    root->addStretch(1);

    auto* btnRow = new QHBoxLayout;
    auto* addBtn = new QPushButton(
        IconLoader::themed(QStringLiteral("login.svg")),
        tr("Add another account…"), this);
    auto* closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setObjectName(QStringLiteral("primary"));
    closeBtn->setDefault(true);
    btnRow->addWidget(addBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    connect(addBtn, &QPushButton::clicked, this, [this] {
        emit addAccountRequested();
        accept();
    });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    // Refresh the account row when the auth layer publishes state
    // changes; covers the case where the user signs out via the row's
    // own button (we keep the dialog open) or a different surface
    // updates the email between dialog construction and actions.
    connect(auth_, &fc::auth::OAuthClient::tokensLoaded,
            this,  &AccountManagerDialog::rebuild);
    connect(auth_, &fc::auth::OAuthClient::granted,
            this,  &AccountManagerDialog::rebuild);
    connect(auth_, &fc::auth::OAuthClient::signedOut,
            this,  &AccountManagerDialog::rebuild);
}

void AccountManagerDialog::rebuild() {
    // Tear the previous row(s) down. Walk takeAt() until empty so both
    // direct widgets and child layouts are cleared cleanly.
    while (auto* item = accountList_->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }

    QString email = auth_->accountEmail();
    if (email.isEmpty()) {
        // Fall back to the active account's persisted email. We resolve
        // the accountId via Database::defaultAccountId — AccountContext
        // (step 4) will own the in-memory current-account selector that
        // makes this lookup explicit.
        const QString aid = fc::cache::Database::defaultAccountId();
        if (!aid.isEmpty()) {
            email = fc::cache::MetaRepository::get(aid,
                                                   QStringLiteral("email"));
        }
    }
    const bool signedIn = auth_->isAuthorized();

    if (!signedIn) {
        auto* none = new QLabel(tr("<i>No account signed in.</i>"), this);
        none->setTextFormat(Qt::RichText);
        accountList_->addWidget(none);
        return;
    }

    auto* row = new QFrame(this);
    row->setObjectName(QStringLiteral("MessageCard"));   // reuses card chrome
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(14, 10, 14, 10);
    rowLayout->setSpacing(10);

    auto* avatar = new QLabel(row);
    avatar->setPixmap(IconLoader::themed(QStringLiteral("user.svg"))
                          .pixmap(28, 28));
    rowLayout->addWidget(avatar, 0, Qt::AlignVCenter);

    auto* identity = new QVBoxLayout;
    identity->setSpacing(2);
    auto* primary = new QLabel(email.isEmpty()
        ? tr("Unknown account")
        : email, row);
    QFont f = primary->font();
    f.setWeight(QFont::DemiBold);
    primary->setFont(f);
    identity->addWidget(primary);
    auto* status = new QLabel(tr("Signed in"), row);
    status->setObjectName(QStringLiteral("FormHint"));
    identity->addWidget(status);
    rowLayout->addLayout(identity, /*stretch=*/1);

    auto* signOutBtn = new QPushButton(
        IconLoader::themed(QStringLiteral("logout.svg")),
        tr("Sign out"), row);
    rowLayout->addWidget(signOutBtn, 0, Qt::AlignVCenter);

    connect(signOutBtn, &QPushButton::clicked, this, [this] {
        emit signOutRequested();
        // Don't accept(): keep the dialog open so the user sees the
        // post-sign-out state ("No account signed in.") and can choose
        // to add another from here.
    });

    accountList_->addWidget(row);
}

}  // namespace fc::ui
