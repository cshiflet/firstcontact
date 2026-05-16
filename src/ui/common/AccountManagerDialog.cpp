#include "AccountManagerDialog.h"

#include "account/AccountContext.h"
#include "account/AccountManager.h"
#include "auth/OAuthClient.h"
#include "cache/Database.h"
#include "cache/MetaRepository.h"
#include "ui/common/IconLoader.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace fc::ui {

AccountManagerDialog::AccountManagerDialog(
        fc::auth::OAuthClient*       auth,
        fc::account::AccountManager* accounts,
        QWidget* parent)
    : QDialog(parent), auth_(auth), accounts_(accounts) {
    setWindowTitle(tr("Manage accounts"));
    resize(560, 400);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(14);

    auto* title = new QLabel(
        tr("<h3 style='margin:0'>Accounts</h3>"), this);
    title->setTextFormat(Qt::RichText);
    root->addWidget(title);

    auto* hint = new QLabel(tr(
        "Sign out preserves the local cache by default — sign back in to "
        "resume without a full re-sync. Use <b>Add another account</b> to "
        "sign in with a second Google account."), this);
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

    // Refresh the rows when the auth layer publishes state changes,
    // and when AccountManager changes (add/remove/setDefault).
    connect(auth_, &fc::auth::OAuthClient::tokensLoaded,
            this,  &AccountManagerDialog::rebuild);
    connect(auth_, &fc::auth::OAuthClient::granted,
            this,  &AccountManagerDialog::rebuild);
    connect(auth_, &fc::auth::OAuthClient::signedOut,
            this,  &AccountManagerDialog::rebuild);
    if (accounts_) {
        connect(accounts_, &fc::account::AccountManager::accountsChanged,
                this,      &AccountManagerDialog::rebuild);
        // Per-context auth state changes — without these the row
        // doesn't update visually when a non-active account signs in
        // / signs out. tokensLoaded covers fresh sign-ins (Add-account
        // adopting tokens) and accountSignedOut covers Sign-out from
        // any row in this dialog.
        connect(accounts_, &fc::account::AccountManager::tokensLoaded,
                this, [this](const QString&) { rebuild(); });
        connect(accounts_, &fc::account::AccountManager::accountSignedOut,
                this, [this](const QString&) { rebuild(); });
    }
}

void AccountManagerDialog::rebuild() {
    while (auto* item = accountList_->takeAt(0)) {
        if (auto* w = item->widget()) {
            // hide() before deleteLater() — without it the widget
            // stays visible inside the dialog (just not in any layout)
            // until the delayed-delete fires. The user saw the new
            // "No accounts signed in." label drawn over the old row
            // because the old row was still painting itself, parented
            // to the dialog, even though the layout had let it go.
            w->hide();
            w->deleteLater();
        }
        if (auto* sub = item->layout()) {
            // Recursively delete sub-layout widgets.
            while (auto* subItem = sub->takeAt(0)) {
                if (auto* w = subItem->widget()) {
                    w->hide();
                    w->deleteLater();
                }
                delete subItem;
            }
            sub->deleteLater();
        }
        delete item;
    }

    // Filter to authorized accounts only. Sign-out can intentionally
    // leave an account row behind so cached messages remain available
    // for cache management, but that row has no active OAuth session.
    // Showing it in Manage accounts would invite the user to sign out
    // of something that is already signed out.
    QList<fc::account::AccountInfo> accounts;
    if (accounts_) {
        for (const auto& a : accounts_->accounts()) {
            auto* ctx = accounts_->contextFor(a.id);
            if (ctx && ctx->auth() && ctx->auth()->isAuthorized()) {
                accounts.append(a);
            }
        }
    }

    if (accounts.isEmpty()) {
        auto* none = new QLabel(tr("<i>No accounts signed in.</i>"), this);
        none->setTextFormat(Qt::RichText);
        accountList_->addWidget(none);
        return;
    }

    auto* group = new QButtonGroup(this);
    group->setExclusive(true);

    for (const auto& a : accounts) {
        auto* row = new QFrame(this);
        row->setObjectName(QStringLiteral("MessageCard"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(14, 10, 14, 10);
        rowLayout->setSpacing(10);

        // v3: per-account accent stripe along the left edge of each
        // row. Hidden when no accent is assigned (default state).
        const QColor accent = fc::account::AccountManager::accentColorFor(
            a.colorHint);
        if (accent.isValid()) {
            auto* stripe = new QFrame(row);
            stripe->setFixedWidth(4);
            stripe->setAutoFillBackground(true);
            QPalette pal = stripe->palette();
            pal.setColor(QPalette::Window, accent);
            stripe->setPalette(pal);
            rowLayout->addWidget(stripe, 0, Qt::AlignVCenter);
        }

        auto* avatar = new QLabel(row);
        avatar->setPixmap(IconLoader::themed(QStringLiteral("user.svg"))
                              .pixmap(28, 28));
        rowLayout->addWidget(avatar, 0, Qt::AlignVCenter);

        auto* identity = new QVBoxLayout;
        identity->setSpacing(2);
        const QString email = a.email.isEmpty()
            ? tr("Unknown account") : a.email;
        auto* primary = new QLabel(email, row);
        QFont f = primary->font();
        f.setWeight(QFont::DemiBold);
        primary->setFont(f);
        identity->addWidget(primary);
        const QString status = a.id == accounts_->currentAccountId()
            ? tr("Active")
            : (a.isDefault ? tr("Default") : tr("Signed in"));
        auto* statusLabel = new QLabel(status, row);
        statusLabel->setObjectName(QStringLiteral("FormHint"));
        identity->addWidget(statusLabel);
        rowLayout->addLayout(identity, /*stretch=*/1);

        auto* defaultRadio = new QRadioButton(tr("Default"), row);
        defaultRadio->setChecked(a.isDefault);
        group->addButton(defaultRadio);
        const QString accountId = a.id;
        connect(defaultRadio, &QRadioButton::toggled, this,
                [this, accountId](bool checked) {
            if (!checked) return;
            if (accounts_) accounts_->setDefault(accountId);
        });
        rowLayout->addWidget(defaultRadio, 0, Qt::AlignVCenter);

        // v3: accent colour picker. A small QComboBox of palette slots,
        // each rendered as a coloured swatch. The first entry is "None"
        // (clears the accent).
        auto* accentCombo = new QComboBox(row);
        {
            auto makeSwatch = [](const QColor& c) {
                QPixmap pm(16, 16);
                pm.fill(Qt::transparent);
                if (c.isValid()) {
                    QPainter p(&pm);
                    p.setRenderHint(QPainter::Antialiasing);
                    p.setBrush(c);
                    p.setPen(Qt::NoPen);
                    p.drawEllipse(2, 2, 12, 12);
                }
                return QIcon(pm);
            };
            accentCombo->addItem(makeSwatch(QColor()), tr("None"), QString());
            for (const QString& slug
                 : fc::account::AccountManager::accentPalette()) {
                accentCombo->addItem(
                    makeSwatch(fc::account::AccountManager::accentColorFor(slug)),
                    slug, slug);
            }
            const int idx = accentCombo->findData(a.colorHint);
            if (idx >= 0) accentCombo->setCurrentIndex(idx);
        }
        connect(accentCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [this, accountId, accentCombo](int) {
            const QString slug = accentCombo->currentData().toString();
            if (accounts_) accounts_->setAccentColor(accountId, slug);
        });
        rowLayout->addWidget(accentCombo, 0, Qt::AlignVCenter);

        auto* signOutBtn = new QPushButton(
            IconLoader::themed(QStringLiteral("logout.svg")),
            tr("Sign out"), row);
        rowLayout->addWidget(signOutBtn, 0, Qt::AlignVCenter);

        connect(signOutBtn, &QPushButton::clicked, this,
                [this, accountId] { emit signOutRequested(accountId); });

        accountList_->addWidget(row);
    }
}

}  // namespace fc::ui
