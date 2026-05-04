#include "MainWindow.h"

#include "api/GmailClient.h"
#include "auth/ClientConfig.h"
#include "auth/OAuthClient.h"
#include "cache/AttachmentRepository.h"
#include "cache/Database.h"
#include "cache/DraftRepository.h"
#include "cache/LabelRepository.h"
#include "cache/MessageRepository.h"
#include "cache/MetaRepository.h"
#include "cache/OutboxRepository.h"
#include "cache/PendingOpsRepository.h"
#include "common/IconLoader.h"
#include "common/Preferences.h"
#include "common/SettingsDialog.h"
#include "common/Shortcuts.h"
#include "common/Theme.h"
#include "compose/ComposeWindow.h"
#include "messagelist/MessageListView.h"
#include "models/LabelTreeModel.h"
#include "models/MessageListModel.h"
#include "reader/ReaderPane.h"
#include "setup/SetupWizard.h"
#include "sidebar/SidebarWidget.h"
#include "sync/DraftSync.h"
#include "sync/OutboxWorker.h"
#include "sync/PendingOpsWorker.h"
#include "sync/SyncService.h"
#include "tray/Notifier.h"
#include "tray/TrayController.h"
#include "util/Browser.h"
#include "util/MimeBuilder.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSize>
#include <QMenu>
#include <QResizeEvent>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace fc::ui {

namespace {
constexpr int kPageSize = 100;
}

MainWindow::MainWindow(fc::auth::ClientConfig* config,
                       fc::auth::OAuthClient* auth,
                       fc::api::GmailClient* gmail,
                       fc::sync::SyncService* sync,
                       fc::sync::OutboxWorker* outbox,
                       fc::sync::PendingOpsWorker* pending,
                       fc::sync::DraftSync* drafts,
                       QWidget* parent)
    : QMainWindow(parent),
      config_(config), auth_(auth), gmail_(gmail),
      sync_(sync), outbox_(outbox), pending_(pending), drafts_(drafts) {
    setWindowTitle(QStringLiteral("FirstContact"));
    resize(1200, 760);
    fc::cache::Database::initialize();

    buildLayout();
    buildToolBar();

    // tray_ and shortcuts_ must be constructed BEFORE wireSignals connects to
    // them — otherwise the QObject::connect calls fire against null pointers.
    tray_      = new TrayController(this, this);
    shortcuts_ = new Shortcuts(this);

    wireSignals();

    statusBar()->showMessage(tr("Ready."));
    refreshAccountIndicator();

    // Re-tint toolbar icons whenever the theme flips so a Settings → Theme
    // change updates immediately instead of waiting for the next launch.
    connect(Theme::instance(), &Theme::changed, this,
            [this](Theme::Mode) { refreshToolbarIcons(); });

    // Hydrate UI from cache without waiting for a network round trip.
    reloadSidebar();
    currentLabelId_ = QStringLiteral("INBOX");
    reloadCurrentLabel();

    // If we have credentials already, kick off background sync.
    if (auth_->isAuthorized()) {
        sync_->runOnce();
        sync_->startScheduler();
        outbox_->start();
        pending_->start();
        drafts_->start();
    }
}

void MainWindow::buildLayout() {
    splitter_ = new QSplitter(Qt::Horizontal, this);

    sidebar_ = new SidebarWidget(splitter_);
    sidebar_->setMaximumWidth(260);

    list_      = new MessageListView(splitter_);
    listModel_ = new fc::MessageListModel(this);
    list_->setModel(listModel_);

    reader_ = new ReaderPane(splitter_);

    splitter_->addWidget(sidebar_);
    splitter_->addWidget(list_);
    splitter_->addWidget(reader_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setStretchFactor(2, 2);

    setCentralWidget(splitter_);
}

void MainWindow::buildToolBar() {
    auto* tb = addToolBar(tr("Main"));
    toolBar_ = tb;
    tb->setMovable(false);
    tb->setIconSize(QSize(18, 18));
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    // Toolbar order, left → right:
    //   Compose, Reply, Reply all, Forward, Delete | Search | Refresh,
    //   Settings, Account
    //
    // Priorities (lower = collapses into the hamburger sooner):
    //   1 Settings, 2 Forward, 3 Delete, 4 Refresh,
    //   5 Reply all, 6 Reply, 7 Compose, 8 Account
    // Search itself is exempt — it shrinks in place down to ~120 px.
    auto withIcon = [this, tb](const QString& svgName, const QString& label,
                                int priority) {
        auto* a = tb->addAction(IconLoader::themed(svgName), label);
        a->setToolTip(label);
        iconActions_.append({a, svgName});
        if (priority > 0) {
            overflowEntries_.push_back({a, /*before=*/nullptr, label, priority});
        }
        return a;
    };

    auto* compose    = withIcon(QStringLiteral("compose.svg"),   tr("Compose"),    7);
    auto* reply      = withIcon(QStringLiteral("reply.svg"),     tr("Reply"),      6);
    auto* replyAll   = withIcon(QStringLiteral("reply-all.svg"), tr("Reply all"),  5);
    auto* forwardAct = withIcon(QStringLiteral("forward.svg"),   tr("Forward"),    2);
    auto* trash      = withIcon(QStringLiteral("trash.svg"),     tr("Delete"),     3);
    tb->addSeparator();

    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(
        tr("Search mail — try from: subject: has:attachment is:unread"));
    searchEdit_->setClearButtonEnabled(true);
    // Let the search bar shrink to ~120 px before any other element gives
    // up its space; below that, overflow logic will decide.
    searchEdit_->setMinimumWidth(120);
    searchEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    searchIconAction_ = searchEdit_->addAction(
        IconLoader::themed(QStringLiteral("search.svg")),
        QLineEdit::LeadingPosition);
    tb->addWidget(searchEdit_);
    tb->addSeparator();

    auto* refresh  = withIcon(QStringLiteral("refresh.svg"),  tr("Refresh"),  4);
    auto* settings = withIcon(QStringLiteral("settings.svg"), tr("Settings"), 1);

    // Account dropdown — mirrors baremail's web UI: a single tool button
    // that pops a menu listing the signed-in email plus Sign out and
    // "Sign in with another account…". Single-account v1 means at most
    // one header row in the menu, but the structure mirrors what we'd
    // grow into for multi-account support later.
    accountButton_ = new QToolButton(tb);
    accountButton_->setIcon(IconLoader::themed(QStringLiteral("user.svg")));
    accountButton_->setPopupMode(QToolButton::InstantPopup);
    accountButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    accountButton_->setAutoRaise(true);
    accountButton_->setCursor(Qt::PointingHandCursor);
    accountMenu_ = new QMenu(accountButton_);
    accountButton_->setMenu(accountMenu_);
    iconActions_.append({accountButton_->defaultAction(), QStringLiteral("user.svg")});
    auto* accountAction = tb->addWidget(accountButton_);
    overflowEntries_.push_back({accountAction, nullptr, tr("Accounts"), 8});
    refreshAccountMenu();

    // Hamburger: hidden until updateToolbarOverflow finds something that
    // doesn't fit. Lives at the very end of the toolbar so it always has
    // a stable anchor; the menu is rebuilt per-resize.
    overflowButton_ = new QToolButton(tb);
    overflowButton_->setIcon(IconLoader::themed(QStringLiteral("menu.svg")));
    overflowButton_->setPopupMode(QToolButton::InstantPopup);
    overflowButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    overflowButton_->setAutoRaise(true);
    overflowButton_->setCursor(Qt::PointingHandCursor);
    overflowButton_->setToolTip(tr("More actions"));
    overflowMenu_ = new QMenu(overflowButton_);
    overflowButton_->setMenu(overflowMenu_);
    overflowAction_ = tb->addWidget(overflowButton_);
    overflowAction_->setVisible(false);

    // Capture each managed action's "anchor" — the toolbar action that
    // appears immediately AFTER it — so updateToolbarOverflow can restore
    // the original order when the window grows back wide enough.
    {
        const auto current = tb->actions();
        for (auto& e : overflowEntries_) {
            const int idx = current.indexOf(e.action);
            e.toolbarBefore = (idx >= 0 && idx + 1 < current.size())
                ? current[idx + 1]
                : nullptr;
        }
    }

    connect(refresh,    &QAction::triggered, this, &MainWindow::onRefresh);
    connect(compose,    &QAction::triggered, this, &MainWindow::onComposeNew);
    connect(reply,      &QAction::triggered, this, &MainWindow::onReplyCurrent);
    connect(replyAll,   &QAction::triggered, this, &MainWindow::onReplyAllCurrent);
    connect(forwardAct, &QAction::triggered, this, &MainWindow::onForwardCurrent);
    connect(trash,      &QAction::triggered, this, &MainWindow::onDeleteCurrent);
    connect(settings,   &QAction::triggered, this, &MainWindow::onOpenSettings);

    connect(searchEdit_, &QLineEdit::returnPressed, this, &MainWindow::onSearchSubmit);
    connect(searchEdit_, &QLineEdit::textChanged,   this, &MainWindow::onSearchChanged);
}

void MainWindow::onOpenSettings() {
    SettingsDialog dlg(this);
    dlg.exec();
    // Settings can flip Preferences::conversationView() (changes the
    // grouping in the message list) and the attachment defaults; the
    // theme listener already lives on Theme::changed. Reload now so
    // changes take effect without needing the user to also click a
    // sidebar entry or refresh.
    reloadCurrentLabel();
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    updateToolbarOverflow();
}

void MainWindow::updateToolbarOverflow() {
    if (!toolBar_ || !overflowAction_) return;

    // 1. Restore everything onto the toolbar in original order.
    for (auto& e : overflowEntries_) {
        toolBar_->removeAction(e.action);
    }
    for (auto& e : overflowEntries_) {
        toolBar_->insertAction(e.toolbarBefore, e.action);
    }
    overflowMenu_->clear();
    overflowAction_->setVisible(false);

    // 2. Force a synchronous layout pass so widgetForAction() and the
    //    toolbar's sizeHint reflect the current set of visible actions.
    if (auto* l = toolBar_->layout()) l->activate();

    auto barFits = [this]() {
        return toolBar_->sizeHint().width() <= toolBar_->width();
    };
    if (barFits()) return;

    // 3. Sort entries by priority ascending — lowest priority collapses
    //    first. Priorities encode the user's desired collapse order:
    //    right-of-search items (Quit=1, Account=2, Settings=3) before any
    //    left-of-search item (Delete=4 → Refresh=9).
    std::vector<OverflowEntry*> order;
    order.reserve(overflowEntries_.size());
    for (auto& e : overflowEntries_) order.push_back(&e);
    std::sort(order.begin(), order.end(),
              [](OverflowEntry* a, OverflowEntry* b) {
                  return a->priority < b->priority;
              });

    // 4. Move actions from the toolbar into the hamburger menu until the
    //    bar fits. We add a *proxy* QAction to the menu (rather than the
    //    real one) because the real action's owner is the toolbar; a
    //    proxy lets the menu show even after we've removed the real
    //    action from the toolbar.
    for (auto* e : order) {
        if (barFits()) break;
        toolBar_->removeAction(e->action);
        if (auto* l = toolBar_->layout()) l->activate();

        QIcon icon;
        if (auto* w = qobject_cast<QToolButton*>(
                toolBar_->widgetForAction(e->action))) {
            icon = w->icon();
        } else {
            icon = e->action->icon();
        }
        auto* proxy = overflowMenu_->addAction(icon, e->text);
        // Forward both Account (a popup-mode QToolButton) and the regular
        // actions: clicking the menu entry triggers the original.
        QPointer<QAction> realAction(e->action);
        connect(proxy, &QAction::triggered, this, [realAction] {
            if (realAction) realAction->trigger();
        });
        overflowAction_->setVisible(true);
    }
}

void MainWindow::refreshToolbarIcons() {
    for (const auto& [action, svg] : iconActions_) {
        if (action) action->setIcon(IconLoader::themed(svg));
    }
    if (searchIconAction_) {
        searchIconAction_->setIcon(IconLoader::themed(QStringLiteral("search.svg")));
    }
}

void MainWindow::wireSignals() {
    connect(list_, &MessageListView::messageActivated,
            this,  &MainWindow::onMessageActivated);
    connect(list_, &MessageListView::starToggled,
            this,  &MainWindow::onToggleStarFor);

    connect(reader_, &ReaderPane::downloadAttachmentRequested,
            this,    &MainWindow::onDownloadAttachment);
    connect(reader_, &ReaderPane::downloadAllRequested,
            this,    &MainWindow::onDownloadAllAttachments);

    connect(sidebar_, &SidebarWidget::labelSelected,
            this,     &MainWindow::onLabelSelected);
    connect(sidebar_, &SidebarWidget::requestCreateLabel,
            this,     &MainWindow::onCreateLabel);
    connect(sidebar_, &SidebarWidget::requestRenameLabel,
            this,     &MainWindow::onRenameLabel);
    connect(sidebar_, &SidebarWidget::requestDeleteLabel,
            this,     &MainWindow::onDeleteLabel);

    connect(auth_, &fc::auth::OAuthClient::granted, this, [this] {
        refreshAccountIndicator();
        sync_->runOnce();
        sync_->startScheduler();
        outbox_->start();
        pending_->start();
        drafts_->start();
    });
    connect(auth_, &fc::auth::OAuthClient::failed, this,
            [this](const QString& reason) {
                QMessageBox::warning(this, tr("Sign-in failed"), reason);
            });
    connect(auth_, &fc::auth::OAuthClient::signedOut, this, [this] {
        refreshAccountIndicator();
        listModel_->replaceAll({});
        reader_->showEmpty();
    });
    connect(auth_, &fc::auth::OAuthClient::browserAuthRequested, this,
            [this](const QUrl& url, bool openedAutomatically) {
                // Always show the dialog with the URL — auto-launch can
                // report success while the launcher (e.g. wslview on a
                // non-standard Windows install) silently fails downstream.
                // The dialog auto-closes once the loopback handler catches
                // the redirect, so successful auto-launches still get the
                // expected one-click flow.
                QApplication::clipboard()->setText(url.toString());

                auto* dlg = new QDialog(this);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                dlg->setWindowTitle(tr("Sign in with Google"));
                dlg->resize(640, 240);

                auto* layout = new QVBoxLayout(dlg);
                const QString headline = openedAutomatically
                    ? tr("<p>We tried to open your browser. "
                         "<b>If a sign-in page didn't appear, paste this URL "
                         "into your browser:</b></p>")
                    : tr("<p>Couldn't launch your default browser "
                         "automatically. <b>Paste this URL into any browser "
                         "to continue.</b></p>");
                const QString footnote = tr(
                    "<p>The URL is already on your clipboard. FirstContact is "
                    "listening on a local port — once you complete consent, "
                    "the redirect will land here and this dialog will close "
                    "itself.</p>");

                auto* msg = new QLabel(headline + footnote, dlg);
                msg->setWordWrap(true);
                msg->setTextFormat(Qt::RichText);
                layout->addWidget(msg);

                auto* urlField = new QLineEdit(url.toString(), dlg);
                urlField->setReadOnly(true);
                urlField->setCursorPosition(0);
                urlField->selectAll();
                layout->addWidget(urlField);

                auto* btnRow = new QHBoxLayout;
                auto* copyBtn  = new QPushButton(tr("Copy URL"),         dlg);
                auto* retryBtn = new QPushButton(tr("Open in Browser"),  dlg);
                auto* closeBtn = new QPushButton(tr("Close"),            dlg);
                btnRow->addWidget(copyBtn);
                btnRow->addWidget(retryBtn);
                btnRow->addStretch(1);
                btnRow->addWidget(closeBtn);
                layout->addLayout(btnRow);

                connect(copyBtn,  &QPushButton::clicked, [url] {
                    QApplication::clipboard()->setText(url.toString());
                });
                connect(retryBtn, &QPushButton::clicked, [url] {
                    // Async — see util::launchBrowser. QDesktopServices::openUrl
                    // here used to block the UI for tens of seconds whenever
                    // xdg-open / wslview was misbehaving.
                    fc::util::launchBrowser(url);
                });
                connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);

                // Auto-close when sign-in completes so the user doesn't have to
                // click anything once Google's redirect comes back.
                connect(auth_, &fc::auth::OAuthClient::granted, dlg, &QDialog::close);
                connect(auth_, &fc::auth::OAuthClient::failed,  dlg, &QDialog::close);

                dlg->show();
            });

    connect(sync_, &fc::sync::SyncService::profileFetched, this,
            [this](const QString& email) {
                auth_->setAccountEmail(email);
                refreshAccountIndicator();
            });
    connect(sync_, &fc::sync::SyncService::labelsUpdated,
            this,  &MainWindow::reloadSidebar);
    connect(sync_, &fc::sync::SyncService::messagesUpdated,
            this,  &MainWindow::reloadCurrentLabel);
    connect(sync_, &fc::sync::SyncService::failed, this,
            [this](const QString& reason) {
                statusBar()->showMessage(tr("Sync error: %1").arg(reason), 5000);
            });
    connect(sync_, &fc::sync::SyncService::newMessages,
            this,  &MainWindow::onNewMessages);

    connect(outbox_, &fc::sync::OutboxWorker::itemSent, this,
            [this](qint64, const QString&) {
                statusBar()->showMessage(tr("Message sent."), 3000);
                sync_->runOnce();
            });
    connect(outbox_, &fc::sync::OutboxWorker::itemFailed, this,
            [this](qint64, const QString& err) {
                statusBar()->showMessage(tr("Send failed: %1").arg(err), 5000);
            });

    connect(pending_, &fc::sync::PendingOpsWorker::itemDropped, this,
            [this](qint64, const QString& reason) {
                // The local optimistic edit no longer matches the server —
                // surface that so the user knows to refresh or re-act.
                statusBar()->showMessage(
                    tr("Server rejected an offline edit (%1). "
                       "Refresh to reconcile.").arg(reason), 6000);
            });
    connect(drafts_, &fc::sync::DraftSync::draftFailed, this,
            [this](const QString&, const QString& reason) {
                statusBar()->showMessage(
                    tr("Draft sync failed: %1").arg(reason), 5000);
            });

    if (tray_) {
        connect(tray_, &TrayController::composeRequested,
                this,  &MainWindow::onComposeNew);
        connect(tray_, &TrayController::refreshRequested,
                this,  &MainWindow::onRefresh);
        connect(tray_, &TrayController::quitRequested,
                qApp,  &QApplication::quit);
        connect(tray_->notifier(), &Notifier::openThreadRequested,
                this,  [this](const QString&) {
                    show(); raise(); activateWindow();
                });
    }

    connect(shortcuts_, &Shortcuts::focusSearch,    this, [this]{ searchEdit_->setFocus(); });
    connect(shortcuts_, &Shortcuts::composeNew,     this, &MainWindow::onComposeNew);
    connect(shortcuts_, &Shortcuts::replyToCurrent,    this, &MainWindow::onReplyCurrent);
    connect(shortcuts_, &Shortcuts::replyAllToCurrent, this, &MainWindow::onReplyAllCurrent);
    connect(shortcuts_, &Shortcuts::forwardCurrent,    this, &MainWindow::onForwardCurrent);
    connect(shortcuts_, &Shortcuts::archiveCurrent, this, &MainWindow::onArchiveCurrent);
    connect(shortcuts_, &Shortcuts::deleteCurrent,  this, &MainWindow::onDeleteCurrent);
    connect(shortcuts_, &Shortcuts::toggleStar,     this, &MainWindow::onToggleStar);
    connect(shortcuts_, &Shortcuts::selectNext, this, [this] {
        const int n = listModel_->rowCount();
        if (n == 0) return;
        const int row = qMin(currentRow_ + 1, n - 1);
        list_->setCurrentIndex(listModel_->index(row, 0));
        onMessageActivated(listModel_->index(row, 0)
                              .data(fc::MessageListModel::IdRole).toString(), row);
    });
    connect(shortcuts_, &Shortcuts::selectPrev, this, [this] {
        if (listModel_->rowCount() == 0) return;
        const int row = qMax(currentRow_ - 1, 0);
        list_->setCurrentIndex(listModel_->index(row, 0));
        onMessageActivated(listModel_->index(row, 0)
                              .data(fc::MessageListModel::IdRole).toString(), row);
    });
    connect(shortcuts_, &Shortcuts::showHelp, this, [this] {
        QMessageBox::information(this, tr("Keyboard shortcuts"),
            tr("/  focus search\n"
               "j  next message\nk  previous message\n"
               "c  compose\nr  reply\nShift+R  reply all\nf  forward\n"
               "e  archive\n#  delete\ns  toggle star\n?  this help"));
    });
}

void MainWindow::refreshAccountIndicator() {
    const QString email = auth_->accountEmail();
    setWindowTitle(email.isEmpty()
        ? QStringLiteral("FirstContact")
        : QStringLiteral("FirstContact — %1").arg(email));
    statusBar()->showMessage(email.isEmpty()
        ? tr("Not signed in.")
        : tr("Signed in as %1").arg(email));
    refreshAccountMenu();
}

void MainWindow::refreshAccountMenu() {
    if (!accountButton_ || !accountMenu_) return;
    accountMenu_->clear();

    // Try the auth layer first; fall back to MetaRepository which the
    // sync layer populates from getProfile. The two can briefly disagree
    // on the very first sign-in (token exchange completes before profile
    // fetch) — the fallback keeps the menu honest in that window.
    QString email = auth_->accountEmail();
    if (email.isEmpty()) email = fc::cache::MetaRepository::get(QStringLiteral("email"));
    const bool signedIn = auth_->isAuthorized();

    accountButton_->setText(tr("Accounts"));
    accountButton_->setToolTip(signedIn && !email.isEmpty()
        ? tr("Signed in as %1").arg(email)
        : tr("Manage Google accounts"));

    if (signedIn) {
        // Header row: non-clickable info line showing the signed-in
        // identity. Empty-email fallback keeps the menu useful while we
        // wait for the first profile fetch to complete.
        const QString headerText = email.isEmpty()
            ? tr("Signed in (email pending sync)")
            : email;
        auto* header = accountMenu_->addAction(headerText);
        header->setEnabled(false);
        accountMenu_->addSeparator();

        auto* signOutAct = accountMenu_->addAction(
            IconLoader::themed(QStringLiteral("logout.svg")),
            email.isEmpty() ? tr("Sign out")
                            : tr("Sign out of %1").arg(email));
        connect(signOutAct, &QAction::triggered, this, &MainWindow::onSignOut);

        accountMenu_->addSeparator();
        // v1 is single-account, so "Sign in with another account…" first
        // signs out the current one. Phase-3+ will turn this into a real
        // multi-account picker; the menu structure is already shaped for it.
        auto* switchAct = accountMenu_->addAction(
            IconLoader::themed(QStringLiteral("login.svg")),
            tr("Sign in with another account…"));
        connect(switchAct, &QAction::triggered, this, &MainWindow::onSwitchAccount);
    } else {
        auto* signInAct = accountMenu_->addAction(
            IconLoader::themed(QStringLiteral("login.svg")),
            tr("Sign in…"));
        connect(signInAct, &QAction::triggered, this, &MainWindow::onSignIn);
    }
}

void MainWindow::onSignIn() {
    if (!config_->isConfigured()) {
        SetupWizard wiz(config_, this);
        if (wiz.exec() != QDialog::Accepted) return;
    }
    statusBar()->showMessage(
        tr("Starting OAuth flow — opening your browser…"), 0);
    auth_->authorize();
}

void MainWindow::onSignOut() {
    if (QMessageBox::question(this, tr("Sign out"),
                              tr("Sign out and clear cached credentials?"))
            == QMessageBox::Yes) {
        auth_->signOut();
        // Clear the cached profile email so the next sign-in's account
        // menu doesn't briefly show the previous user's address before
        // the new initial sync completes.
        fc::cache::MetaRepository::set(QStringLiteral("email"), QString());
        sync_->stopScheduler();
        outbox_->stop();
        pending_->stop();
        drafts_->stop();
    }
}

void MainWindow::onSwitchAccount() {
    // v1 is single-account: switching means signing out the current account
    // and starting the OAuth flow against whatever account the user picks
    // in Google's consent screen. Confirm explicitly so a stray click on
    // the menu doesn't drop the active session.
    if (auth_->isAuthorized()) {
        const QString current = auth_->accountEmail();
        const QString prompt = current.isEmpty()
            ? tr("Sign out of the current account and sign in to a different one?")
            : tr("Sign out of %1 and sign in to a different account?").arg(current);
        if (QMessageBox::question(this, tr("Switch account"), prompt)
                != QMessageBox::Yes) {
            return;
        }
        auth_->signOut();
        fc::cache::MetaRepository::set(QStringLiteral("email"), QString());
        sync_->stopScheduler();
        outbox_->stop();
        pending_->stop();
        drafts_->stop();
    }
    onSignIn();
}

void MainWindow::onRefresh() {
    if (!auth_->isAuthorized()) {
        onSignIn();
        return;
    }
    statusBar()->showMessage(tr("Syncing…"));
    sync_->runOnce();
}

void MainWindow::onLabelSelected(const QString& id) {
    if (id.isEmpty() || id == currentLabelId_) return;
    currentLabelId_ = id;
    reloadCurrentLabel();
}

void MainWindow::reloadSidebar() {
    sidebar_->model()->reload();
}

void MainWindow::reloadCurrentLabel() {
    const bool conv = Preferences::conversationView();
    auto rows = currentSearchQuery_.isEmpty()
        ? (conv
            ? fc::cache::MessageRepository::listThreadsByLabel(currentLabelId_, kPageSize, 0)
            : fc::cache::MessageRepository::listByLabel(currentLabelId_, kPageSize, 0))
        : (conv
            ? fc::cache::MessageRepository::searchFtsThreads(currentSearchQuery_, kPageSize)
            : fc::cache::MessageRepository::searchFts(currentSearchQuery_, kPageSize));
    listModel_->replaceAll(std::move(rows));
    currentRow_ = -1;
    reader_->showEmpty();
    statusBar()->showMessage(currentSearchQuery_.isEmpty()
        ? tr("Showing %1 messages in %2").arg(listModel_->rowCount()).arg(currentLabelId_)
        : tr("Search results: %1").arg(listModel_->rowCount()));
}

void MainWindow::onMessageActivated(const QString& messageId, int row) {
    if (messageId.isEmpty()) return;
    currentRow_ = row;

    fc::Message cached = fc::cache::MessageRepository::byId(messageId);

    auto renderThread = [this](const fc::Message& selected) {
        currentMessage_ = selected;
        auto thread = fc::cache::MessageRepository::byThread(selected.threadId);
        if (thread.size() > 1) {
            reader_->showThread(thread);
        } else {
            reader_->showMessage(selected);
        }
        fc::cache::MessageRepository::markAccessed(selected.id);
    };

    // Cache shortcut: only skip the network round-trip when the cached row
    // looks complete. A row that has body_text but is missing body_html
    // despite advertising body_html_present is incomplete — typically a
    // pre-migration-v3 row from before we persisted the HTML body. Same
    // reasoning for hasAttachment with no rows in the attachments table:
    // pre-AttachmentRepository caches don't have any attachment rows even
    // when the message advertises has_attachment=1. Re-fetch so the
    // attachment chips and "Open in browser" / inline preview work on
    // the next click.
    const bool cacheLooksComplete =
        !cached.id.isEmpty()
        && !cached.bodyText.isEmpty()
        && (!cached.bodyHtmlPresent || !cached.bodyHtml.isEmpty())
        && (!cached.hasAttachment   || !cached.attachments.empty());

    if (cacheLooksComplete) {
        renderThread(cached);
        return;
    }

    reader_->showLoading();
    QPointer<MainWindow> self(this);
    gmail_->getMessage(messageId,
        [self, messageId, renderThread](fc::Message m, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                self->reader_->showEmpty(tr("Failed to load: %1").arg(err.message));
                return;
            }
            fc::cache::MessageRepository::upsert(m);
            renderThread(m);
        });
}

void MainWindow::onSearchChanged() {
    // Live search against the local FTS5 index — fast even on large caches.
    currentSearchQuery_ = searchEdit_->text().trimmed();
    reloadCurrentLabel();
}

void MainWindow::onSearchSubmit() {
    // Enter triggers a server-side search using Gmail's `q` syntax to find
    // anything not in the local cache yet, then merge into the model.
    const QString q = searchEdit_->text().trimmed();
    if (q.isEmpty() || !auth_->isAuthorized()) return;
    statusBar()->showMessage(tr("Searching server: %1").arg(q));
    QPointer<MainWindow> self(this);
    gmail_->listMessages({}, q, {}, kPageSize,
        [self, gmail = gmail_, q](fc::api::GmailClient::ListPage page,
                                  fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                self->statusBar()->showMessage(
                    tr("Server search failed: %1").arg(err.message));
                return;
            }
            // Hydrate any missing ids; the cache will then surface them.
            for (const auto& id : page.ids) {
                if (fc::cache::MessageRepository::exists(id)) continue;
                gmail->getMessage(id,
                    [](fc::Message m, fc::api::ApiError gErr) {
                        if (!gErr) fc::cache::MessageRepository::upsert(m);
                    });
            }
            self->statusBar()->showMessage(
                tr("Server returned %1 ids; cache updates in background.")
                    .arg(page.ids.size()));
            self->currentSearchQuery_ = q;
            self->reloadCurrentLabel();
        });
}

void MainWindow::openComposeWindow(const fc::Message* parent, int mode) {
    if (!auth_->isAuthorized()) { onSignIn(); return; }

    auto* w = new ComposeWindow(auth_->accountEmail(), QString(), this);
    w->setAttribute(Qt::WA_DeleteOnClose);
    if (parent) {
        w->prefillFrom(*parent, static_cast<ComposeWindow::Mode>(mode));
    }
    connect(w, &ComposeWindow::composeReady, this,
        [this](const fc::util::OutgoingMessage& msg, const QString& threadId) {
            const QByteArray rfc = fc::util::MimeBuilder::build(msg);
            fc::cache::OutboxItem item;
            item.rfc5322  = rfc;
            item.threadId = threadId;
            fc::cache::OutboxRepository::enqueue(item);
            outbox_->flush();
        });
    connect(w, &ComposeWindow::saveDraftRequested, this,
        [this, w](const fc::util::OutgoingMessage& msg, const QString& threadId,
                  const QString& existingDraftId) {
            fc::cache::DraftRow row;
            row.id                 = existingDraftId;
            row.threadId           = threadId;
            row.subject            = msg.subject;
            row.toAddrs            = msg.to;
            row.ccAddrs            = msg.cc;
            row.bccAddrs           = msg.bcc;
            row.bodyText           = msg.bodyText;
            row.inReplyToMessageId = msg.rfc822InReplyTo;
            row.dirty              = true;
            const QString id = fc::cache::DraftRepository::upsert(row);
            // Thread the assigned id back into the still-open compose window
            // so the next save updates instead of creating a new draft.
            w->loadFromDraft(id, threadId, msg.subject, msg.to, msg.cc, msg.bodyText);
            drafts_->flush();
            statusBar()->showMessage(tr("Draft saved."), 2000);
        });
    w->show();
}

void MainWindow::onComposeNew() {
    openComposeWindow(nullptr, int(ComposeWindow::Mode::New));
}

void MainWindow::onReplyCurrent() {
    if (currentMessage_.id.isEmpty()) return;
    openComposeWindow(&currentMessage_, int(ComposeWindow::Mode::Reply));
}

void MainWindow::onReplyAllCurrent() {
    if (currentMessage_.id.isEmpty()) return;
    openComposeWindow(&currentMessage_, int(ComposeWindow::Mode::ReplyAll));
}

void MainWindow::onForwardCurrent() {
    if (currentMessage_.id.isEmpty()) return;
    openComposeWindow(&currentMessage_, int(ComposeWindow::Mode::Forward));
}

void MainWindow::onCreateLabel(const QString& parentLabelId) {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New label"),
        tr("Label name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.isEmpty()) return;

    const auto parent = fc::cache::LabelRepository::byId(parentLabelId);
    if (!parent.id.isEmpty() && parent.type == QLatin1String("user")) {
        name = parent.name + QLatin1Char('/') + name;
    }
    QPointer<MainWindow> self(this);
    gmail_->createLabel(name,
        [self](fc::api::GmailClient::Label l, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                QMessageBox::warning(self, tr("Create label"), err.message);
                return;
            }
            fc::cache::LabelRow row;
            row.id   = l.id;
            row.name = l.name;
            row.type = l.type;
            fc::cache::LabelRepository::upsert(row);
            self->reloadSidebar();
        });
}

void MainWindow::onRenameLabel(const QString& labelId) {
    const auto current = fc::cache::LabelRepository::byId(labelId);
    if (current.id.isEmpty() || current.type != QLatin1String("user")) return;

    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("Rename label"),
        tr("New name:"), QLineEdit::Normal, current.name, &ok);
    if (!ok || newName.isEmpty() || newName == current.name) return;

    QPointer<MainWindow> self(this);
    gmail_->updateLabel(labelId, newName,
        [self, labelId, newName](fc::api::GmailClient::Label, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                QMessageBox::warning(self, tr("Rename label"), err.message);
                return;
            }
            auto row = fc::cache::LabelRepository::byId(labelId);
            row.name = newName;
            fc::cache::LabelRepository::upsert(row);
            self->reloadSidebar();
        });
}

void MainWindow::onDeleteLabel(const QString& labelId) {
    const auto current = fc::cache::LabelRepository::byId(labelId);
    if (current.id.isEmpty() || current.type != QLatin1String("user")) return;
    if (QMessageBox::question(this, tr("Delete label"),
            tr("Delete the label '%1'? Messages keep their other labels.")
              .arg(current.name)) != QMessageBox::Yes) return;

    QPointer<MainWindow> self(this);
    gmail_->deleteLabel(labelId,
        [self, labelId](fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                QMessageBox::warning(self, tr("Delete label"), err.message);
                return;
            }
            fc::cache::LabelRepository::remove(labelId);
            self->reloadSidebar();
        });
}

void MainWindow::onToggleStar() {
    if (currentMessage_.id.isEmpty()) return;
    onToggleStarFor(currentMessage_.id);
}

void MainWindow::onToggleStarFor(const QString& messageId) {
    if (messageId.isEmpty()) return;

    // Re-read from cache so we toggle relative to the row the user clicked,
    // not to whatever currentMessage_ happens to be (the click target may
    // not be the currently-selected row).
    const fc::Message m = fc::cache::MessageRepository::byId(messageId);
    if (m.id.isEmpty()) return;

    QStringList add, rem;
    if (m.isStarred) rem << QStringLiteral("STARRED");
    else             add << QStringLiteral("STARRED");

    fc::cache::MessageRepository::applyLabelDiff(messageId, add, rem);
    fc::cache::PendingOpsRepository::enqueueModify(messageId, add, rem);
    pending_->flush();

    // Keep currentMessage_ in sync if the toggle was on the currently-shown
    // row, so the next keyboard-bound toggle (or anything else reading
    // currentMessage_) sees the new state.
    if (currentMessage_.id == messageId) currentMessage_.isStarred = !m.isStarred;
    reloadCurrentLabel();
}

namespace {

// Returns a path under `dir` for `filename` that doesn't already exist,
// inserting " (1)", " (2)", … before the suffix as needed. Used by the
// non-picker download paths so we never silently overwrite an existing
// file the user may still need.
QString uniqueTargetPath(const QString& dir, const QString& filename) {
    const QString safe = filename.isEmpty()
        ? QStringLiteral("attachment.bin")
        : QFileInfo(filename).fileName();
    QString target = dir + QLatin1Char('/') + safe;
    for (int i = 1; QFileInfo::exists(target) && i < 1000; ++i) {
        const QFileInfo fi(safe);
        target = QStringLiteral("%1/%2 (%3)%4%5")
            .arg(dir,
                 fi.completeBaseName(),
                 QString::number(i),
                 fi.suffix().isEmpty() ? QString() : QStringLiteral("."),
                 fi.suffix());
    }
    return target;
}

// Writes bytes to absolute path. Returns true on full write. Caller is
// responsible for surfacing the error path.
bool writeBytesToPath(const QString& path, const QByteArray& bytes,
                      QString* errOut) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errOut) *errOut = f.errorString();
        return false;
    }
    const auto written = f.write(bytes);
    f.close();
    if (written != bytes.size()) {
        if (errOut) *errOut = QObject::tr("short write");
        return false;
    }
    return true;
}

}  // namespace

void MainWindow::onDownloadAttachment(const QString& messageId,
                                      const QString& attachmentId,
                                      const QString& filename,
                                      bool forceSaveAs) {
    if (messageId.isEmpty() || attachmentId.isEmpty()) {
        statusBar()->showMessage(tr("Attachment is not downloadable."), 5000);
        return;
    }
    if (!auth_->isAuthorized()) {
        statusBar()->showMessage(
            tr("Sign in to download attachments."), 5000);
        return;
    }

    // Resolve the destination NOW (synchronously, before the network call)
    // so the file picker is a direct response to the user's gesture.
    // Showing it inside the gmail_->getAttachment callback would feel
    // disconnected — by the time bytes arrive, the user has moved on.
    const bool ask = forceSaveAs || Preferences::alwaysAskAttachmentLocation();
    QString target;
    if (ask) {
        const QString suggested = Preferences::attachmentDir()
            + QLatin1Char('/')
            + (filename.isEmpty() ? QStringLiteral("attachment.bin")
                                  : QFileInfo(filename).fileName());
        target = QFileDialog::getSaveFileName(
            this, tr("Save attachment"), suggested);
        if (target.isEmpty()) {
            statusBar()->showMessage(tr("Download cancelled."), 3000);
            return;
        }
    } else {
        const QString dir = Preferences::attachmentDir();
        QDir().mkpath(dir);
        target = uniqueTargetPath(dir, filename);
    }

    statusBar()->showMessage(tr("Downloading %1…").arg(filename));
    QPointer<MainWindow> self(this);
    gmail_->getAttachment(messageId, attachmentId,
        [self, attachmentId, target](QByteArray bytes, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                self->statusBar()->showMessage(
                    tr("Download failed: %1").arg(err.message), 8000);
                return;
            }
            QString writeErr;
            if (!writeBytesToPath(target, bytes, &writeErr)) {
                self->statusBar()->showMessage(
                    tr("Couldn't write %1: %2").arg(target, writeErr), 8000);
                return;
            }
            fc::cache::AttachmentRepository::markDownloaded(attachmentId, target);
            self->statusBar()->showMessage(tr("Saved to %1").arg(target), 8000);
            QDesktopServices::openUrl(QUrl::fromLocalFile(target));
        });
}

void MainWindow::onDownloadAllAttachments(const QString& messageId) {
    if (messageId.isEmpty()) return;
    if (!auth_->isAuthorized()) {
        statusBar()->showMessage(
            tr("Sign in to download attachments."), 5000);
        return;
    }

    const auto m = fc::cache::MessageRepository::byId(messageId);
    if (m.attachments.empty()) {
        statusBar()->showMessage(tr("No attachments on this message."), 4000);
        return;
    }

    // For "Download all" the natural unit is a folder, not a per-file picker
    // (asking N times in a row is hostile UX). When always-ask is on, ask
    // once for a folder; otherwise use the configured default. Each file
    // gets uniqueTargetPath collision handling within that folder.
    QString dir;
    if (Preferences::alwaysAskAttachmentLocation()) {
        dir = QFileDialog::getExistingDirectory(
            this, tr("Save all attachments to folder"),
            Preferences::attachmentDir());
        if (dir.isEmpty()) {
            statusBar()->showMessage(tr("Download cancelled."), 3000);
            return;
        }
    } else {
        dir = Preferences::attachmentDir();
    }
    QDir().mkpath(dir);

    int kicked = 0;
    QPointer<MainWindow> self(this);
    for (const auto& a : m.attachments) {
        if (a.id.isEmpty()) continue;   // inline-only; not addressable
        const QString target = uniqueTargetPath(dir, a.filename);
        const QString attachmentId = a.id;
        const QString filename     = a.filename;
        gmail_->getAttachment(messageId, attachmentId,
            [self, attachmentId, target, filename](
                    QByteArray bytes, fc::api::ApiError err) {
                if (!self) return;
                if (err) {
                    self->statusBar()->showMessage(
                        tr("Download failed (%1): %2").arg(filename, err.message),
                        8000);
                    return;
                }
                QString writeErr;
                if (!writeBytesToPath(target, bytes, &writeErr)) {
                    self->statusBar()->showMessage(
                        tr("Couldn't write %1: %2").arg(target, writeErr), 8000);
                    return;
                }
                fc::cache::AttachmentRepository::markDownloaded(attachmentId, target);
                self->statusBar()->showMessage(
                    tr("Saved %1").arg(QFileInfo(target).fileName()), 4000);
            });
        ++kicked;
    }

    if (kicked == 0) {
        statusBar()->showMessage(
            tr("No downloadable attachments on this message."), 4000);
    } else {
        statusBar()->showMessage(
            tr("Downloading %1 attachments to %2…").arg(kicked).arg(dir));
    }
}

void MainWindow::onDeleteCurrent() {
    if (currentMessage_.id.isEmpty()) return;
    if (QMessageBox::question(this, tr("Move to Trash"),
            tr("Move this message to Trash?"),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    // Gmail's "trash" is a label transition: drop INBOX (and any system
    // categories) and add TRASH. The pending-ops worker reconciles via
    // messages.modify, which Gmail accepts as the canonical move-to-trash
    // action. We deliberately don't call the dedicated /trash endpoint
    // because applyLabelDiff already supports the offline-edit path.
    const QString id = currentMessage_.id;
    const QStringList add{QStringLiteral("TRASH")};
    const QStringList rem{QStringLiteral("INBOX")};
    fc::cache::MessageRepository::applyLabelDiff(id, add, rem);
    fc::cache::PendingOpsRepository::enqueueModify(id, add, rem);
    pending_->flush();
    statusBar()->showMessage(tr("Moved to Trash."), 3000);
    reloadCurrentLabel();
}

void MainWindow::onArchiveCurrent() {
    if (currentMessage_.id.isEmpty()) return;
    const QStringList rem{QStringLiteral("INBOX")};
    fc::cache::MessageRepository::applyLabelDiff(currentMessage_.id, {}, rem);
    fc::cache::PendingOpsRepository::enqueueModify(currentMessage_.id, {}, rem);
    pending_->flush();
    reloadCurrentLabel();
}

void MainWindow::onNewMessages(int count) {
    if (count <= 0 || !tray_ || !tray_->notifier()) return;

    // Pull the most recent message we just upserted to populate the toast.
    auto recent = fc::cache::MessageRepository::listByLabel(
        QStringLiteral("INBOX"), 1, 0);
    QString sender, subject, threadId;
    if (!recent.empty()) {
        sender   = recent.front().fromName.isEmpty()
                     ? recent.front().fromAddr
                     : recent.front().fromName;
        subject  = recent.front().subject;
        threadId = recent.front().threadId;
    }
    tray_->notifier()->notifyNewMail(count, sender, subject, threadId);
}

}  // namespace fc::ui
