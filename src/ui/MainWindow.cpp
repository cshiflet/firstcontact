#include "MainWindow.h"

#include "account/AccountContext.h"
#include "account/AccountManager.h"
#include "api/GmailClient.h"
#include "api/SessionTransfer.h"
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
#include "common/AccountManagerDialog.h"
#include "common/CacheManagerDialog.h"
#include "common/IconLoader.h"
#include "common/LabelChooserDialog.h"
#include "common/LabelStyleCache.h"
#include "common/Preferences.h"
#include "common/SettingsDialog.h"
#include "common/Shortcuts.h"
#include "common/SpinningToolButton.h"
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
#include "util/DryRun.h"
#include "util/MimeBuilder.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace fc::cache { QSqlDatabase databaseHandle(); }

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QTimer>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSize>
#include <QMenu>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
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
                       fc::account::AccountManager* accounts,
                       QWidget* parent)
    : QMainWindow(parent),
      config_(config), auth_(auth), gmail_(gmail),
      sync_(sync), outbox_(outbox), pending_(pending), drafts_(drafts),
      accounts_(accounts) {
    setWindowTitle(QStringLiteral("FirstContact"));
    resize(1200, 760);
    fc::cache::Database::initialize();

    // Seed the active account from AccountManager's selection (which
    // applies the same default-account rule as Database::defaultAccountId,
    // but in-memory). Step 8 wires currentAccountChanged to UI repaints
    // so the toolbar account menu can flip the panes between accounts.
    currentAccountId_ = accounts_->currentAccountId();
    sync_->setAccountId(currentAccountId_);

    buildLayout();
    buildToolBar();

    // tray_ and shortcuts_ must be constructed BEFORE wireSignals connects to
    // them — otherwise the QObject::connect calls fire against null pointers.
    tray_      = new TrayController(this, this);
    shortcuts_ = new Shortcuts(this);

    // Persistent error chip. Lives on the right side of the status bar
    // (addPermanentWidget), hidden until a failure puts something in
    // it. Sticks until the user dismisses it OR a subsequent sync
    // starts — in either case we want the red glyph to draw the eye
    // away from the rest of the chrome until the user has actually
    // seen the message.
    errorBanner_ = new QFrame(this);
    errorBanner_->setObjectName(QStringLiteral("ErrorBanner"));
    errorBanner_->hide();
    {
        auto* row = new QHBoxLayout(errorBanner_);
        row->setContentsMargins(8, 1, 4, 1);
        row->setSpacing(6);
        auto* icon = new QLabel(QStringLiteral("⚠"), errorBanner_);
        icon->setObjectName(QStringLiteral("ErrorBannerIcon"));
        errorBannerLabel_ = new QLabel(errorBanner_);
        errorBannerLabel_->setObjectName(QStringLiteral("ErrorBannerText"));
        errorBannerLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* dismiss = new QPushButton(QStringLiteral("✕"), errorBanner_);
        dismiss->setObjectName(QStringLiteral("ErrorBannerClose"));
        dismiss->setFlat(true);
        dismiss->setFixedSize(20, 20);
        dismiss->setToolTip(tr("Dismiss"));
        // Button text "✕" reads to screen readers as "x" — give it a
        // proper name. Description echoes the banner role so the user
        // knows what they're dismissing.
        dismiss->setAccessibleName(tr("Dismiss error"));
        dismiss->setAccessibleDescription(
            tr("Hide the error banner above. Does not retry the failed action."));
        dismiss->setCursor(Qt::PointingHandCursor);
        row->addWidget(icon);
        row->addWidget(errorBannerLabel_, /*stretch=*/1);
        row->addWidget(dismiss);
        connect(dismiss, &QPushButton::clicked, errorBanner_,
                &QWidget::hide);
    }
    statusBar()->addPermanentWidget(errorBanner_);

    // Bandwidth meter — small "↓ 0 B" pill that grows with the session's
    // accumulated wire transfer. Useful on metered or slow links to
    // see what the app is actually costing. Tooltip explains the
    // breakdown. Sits permanently on the right of the status bar.
    bandwidthLabel_ = new QLabel(this);
    bandwidthLabel_->setObjectName(QStringLiteral("FormHint"));
    bandwidthLabel_->setContentsMargins(8, 0, 8, 0);
    statusBar()->addPermanentWidget(bandwidthLabel_);
    refreshBandwidthLabel();
    connect(&fc::api::SessionTransfer::instance(),
            &fc::api::SessionTransfer::changed,
            this, &MainWindow::refreshBandwidthLabel,
            Qt::QueuedConnection);

    wireSignals();

    // Initial baseline. The sync handler installed in wireSignals
    // overrides via showMessage when a sync starts, and the
    // messageChanged restorer below brings this baseline back when
    // any transient (sync done, archived, saved, etc.) clears.
    refreshAccountIndicator();
    if (fc::util::DryRun::enabled()) {
        statusBar()->showMessage(
            tr("Dry-run mode enabled (FC_DRY_RUN). "
               "All destructive operations are blocked."), 8000);
    }

    // Re-tint toolbar icons whenever the theme flips so a Settings → Theme
    // change updates immediately instead of waiting for the next launch.
    connect(Theme::instance(), &Theme::changed, this,
            [this](Theme::Mode) { refreshToolbarIcons(); });

    // Hydrate UI from cache without waiting for a network round trip —
    // BUT only when the active account is actually signed in. The
    // accounts table can carry rows for accounts that signed in once
    // and signed out (we keep the row + cache when the user said "keep
    // cached data" so a re-sign-in is fast); migration 0006 also seeds
    // a synthetic legacy row. Without this gate, a launched-with-no-
    // signed-in-account window would render the previous account's
    // mail straight from cache. enforceActiveAccountGate() runs the
    // check now (best-effort — keychain hydration is async, so this
    // first call may say "not signed in" even when we will be) AND
    // again from the tokensLoaded handler once the per-account
    // OAuthClients have finished hydrating from the keychain.
    enforceActiveAccountGate();
    if (auth_->isAuthorized()) {
        reloadSidebar();
        currentLabelId_ = QStringLiteral("INBOX");
        reloadCurrentLabel();
    }

    // Snooze wake-up scheduler. Snooze is FirstContact-local — Gmail's
    // API doesn't expose its native snooze, so we tick every 60 s
    // ourselves to find rows whose snooze_until has lapsed and restore
    // INBOX on them. Run once now so a freshly-launched app catches
    // anything that became due while it was closed.
    {
        wakeDueSnoozedMessages();
        auto* snoozeTimer = new QTimer(this);
        snoozeTimer->setInterval(60'000);
        connect(snoozeTimer, &QTimer::timeout,
                this, &MainWindow::wakeDueSnoozedMessages);
        snoozeTimer->start();
    }

    // If we have credentials already, kick off background sync. With
    // per-account contexts (step 6+), each account runs its own
    // SyncService timer so a stuck account doesn't starve others; the
    // workers stay shared because their drain queries run cross-account
    // and dispatch via the GmailResolver.
    if (auth_->isAuthorized()) {
        for (auto* ctx : accounts_->allContexts()) {
            if (auto* s = ctx->sync()) {
                s->runOnce();
                s->startScheduler();
            }
        }
        // If no contexts (anonymous fallback) fire the legacy sync
        // path so the sign-in flow still completes.
        if (accounts_->allContexts().isEmpty() && sync_) {
            sync_->runOnce();
            sync_->startScheduler();
        }
        outbox_->start();
        pending_->start();
        drafts_->start();
    }
}

void MainWindow::buildLayout() {
    splitter_ = new QSplitter(Qt::Horizontal, this);

    sidebar_ = new SidebarWidget(splitter_);
    sidebar_->setMaximumWidth(260);

    // Wrap the list view + a small footer label in a single column so
    // the footer ("Loading more messages…" / "No more messages") sits
    // tight beneath the list within the splitter cell.
    auto* listColumn = new QWidget(splitter_);
    auto* listColumnLayout = new QVBoxLayout(listColumn);
    listColumnLayout->setContentsMargins(0, 0, 0, 0);
    listColumnLayout->setSpacing(0);

    list_      = new MessageListView(listColumn);
    listModel_ = new fc::MessageListModel(this);
    list_->setModel(listModel_);
    listColumnLayout->addWidget(list_, /*stretch=*/1);

    listFooter_ = new QLabel(listColumn);
    listFooter_->setObjectName(QStringLiteral("listFooter"));
    listFooter_->setAlignment(Qt::AlignCenter);
    listFooter_->setContentsMargins(8, 6, 8, 6);
    listFooter_->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
    listFooter_->setVisible(false);
    listColumnLayout->addWidget(listFooter_);

    reader_ = new ReaderPane(splitter_);

    splitter_->addWidget(sidebar_);
    splitter_->addWidget(listColumn);
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
    tb->setToolButtonStyle(Preferences::toolbarShowText()
        ? Qt::ToolButtonTextBesideIcon
        : Qt::ToolButtonIconOnly);

    // Toolbar order, left → right:
    //   Compose, Reply, Reply all, Forward, Delete | Search | Refresh,
    //   Settings, Account
    //
    // Priorities (lower = collapses into the hamburger sooner):
    //   1 Settings, 2 Forward, 3 Delete, 4 Refresh,
    //   5 Reply all, 6 Reply, 7 Compose, 8 Account
    // Search itself is exempt — it shrinks in place down to ~120 px.
    //
    // Shortcut hint: appended to the tooltip in parens. Empty string
    // means the action has no associated keybind.
    auto withIcon = [this, tb](const QString& svgName, const QString& label,
                                int priority,
                                const QString& shortcutHint = QString()) {
        auto* a = tb->addAction(IconLoader::themed(svgName), label);
        a->setToolTip(shortcutHint.isEmpty()
            ? label
            : QStringLiteral("%1 (%2)").arg(label, shortcutHint));
        iconActions_.append({a, svgName});
        if (priority > 0) {
            overflowEntries_.push_back({a, /*before=*/nullptr, label, priority, {}, {}});
        }
        return a;
    };

    auto* compose    = withIcon(QStringLiteral("compose.svg"),   tr("Compose"),    7,
                                 QStringLiteral("c"));
    auto* reply      = withIcon(QStringLiteral("reply.svg"),     tr("Reply"),      6,
                                 QStringLiteral("r"));
    auto* replyAll   = withIcon(QStringLiteral("reply-all.svg"), tr("Reply all"),  5,
                                 QStringLiteral("a"));
    auto* forwardAct = withIcon(QStringLiteral("forward.svg"),   tr("Forward"),    2,
                                 QStringLiteral("f"));
    auto* archiveAct = withIcon(QStringLiteral("archive.svg"),   tr("Archive"),    3,
                                 QStringLiteral("e"));
    auto* readAct    = withIcon(QStringLiteral("mark-read.svg"), tr("Mark read/unread"), 2,
                                 tr("Shift+I / Shift+U"));
    auto* snoozeAct  = withIcon(QStringLiteral("snooze.svg"),    tr("Snooze"),     2,
                                 QStringLiteral("b"));
    auto* trash      = withIcon(QStringLiteral("trash.svg"),     tr("Delete"),     2,
                                 QStringLiteral("#"));

    // Filter chip: All / Unread-only. Checkable; persists in
    // Preferences (matches Gmail web's filter-chip behaviour). Lives
    // between the action cluster and the search bar so it reads as
    // "what's IN this view" rather than as an action on the
    // selection. Lower priority so it falls into the hamburger menu
    // sooner than archive / delete on narrow windows.
    auto* unreadAct = tb->addAction(IconLoader::themed(
                                         QStringLiteral("mark-read.svg")),
                                     tr("Unread only"));
    unreadAct->setCheckable(true);
    unreadAct->setChecked(Preferences::unreadOnly());
    unreadAct->setToolTip(tr(
        "Filter the message list to unread messages (or threads with "
        "any unread message in conversation view)."));
    iconActions_.append({unreadAct, QStringLiteral("mark-read.svg")});
    overflowEntries_.push_back({unreadAct, /*before=*/nullptr,
                                  tr("Unread only"), 2, {}, {}});
    connect(unreadAct, &QAction::toggled, this, [this](bool on) {
        Preferences::setUnreadOnly(on);
        reloadCurrentLabel();
    });

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
    QAction* searchAction = tb->addWidget(searchEdit_);
    // Search collapses LAST — only after every icon button has already
    // moved into the hamburger. Custom menuTrigger because the toolbar
    // representation is a QLineEdit widget (not a triggerable QAction):
    // the menu entry pops a small input dialog seeded with the current
    // query, then submits via onSearchSubmit.
    overflowEntries_.push_back({
        searchAction, nullptr, tr("Search…"), 9,
        [this]() {
            bool ok = false;
            const QString q = QInputDialog::getText(this, tr("Search mail"),
                tr("Query (Gmail syntax — try from: subject: has:attachment "
                   "is:unread):"),
                QLineEdit::Normal, searchEdit_->text(), &ok);
            if (!ok) return;
            searchEdit_->setText(q);
            if (q.trimmed().isEmpty()) onSearchChanged();
            else                       onSearchSubmit();
        },
        QStringLiteral("search.svg"),
    });
    tb->addSeparator();

    // Refresh: a SpinningToolButton instead of the auto-created QToolButton
    // from withIcon, so the icon can rotate during sync. We still go
    // through a real QAction (for shortcut + menu reuse), but the toolbar
    // hosts our subclass directly via addWidget. Tradeoff: refresh no
    // longer overflows into the hamburger when the toolbar is narrow —
    // acceptable, since refresh is the most-used action and the spinner
    // wouldn't render meaningfully in a menu item anyway.
    auto* refresh = new QAction(IconLoader::themed(QStringLiteral("refresh.svg")),
                                 tr("Refresh"), this);
    refresh->setToolTip(tr("Refresh"));
    syncBtn_ = new SpinningToolButton(tb);
    syncBtn_->setDefaultAction(refresh);
    syncBtn_->setBaseIcon(IconLoader::themed(QStringLiteral("refresh.svg")));
    syncBtn_->setAutoRaise(true);
    syncBtn_->setToolButtonStyle(Preferences::toolbarShowText()
        ? Qt::ToolButtonTextBesideIcon
        : Qt::ToolButtonIconOnly);
    syncBtn_->setCursor(Qt::PointingHandCursor);
    syncBtn_->setAccessibleName(tr("Refresh"));
    syncBtn_->setAccessibleDescription(tr(
        "Trigger a sync now. The icon spins while a sync is in flight."));
    tb->addWidget(syncBtn_);
    iconActions_.append({refresh, QStringLiteral("refresh.svg")});

    auto* settings = withIcon(QStringLiteral("settings.svg"), tr("Settings"), 1);
    // Search bar's leading-icon tooltip — same pattern: pressing `/`
    // anywhere in the window pops focus back into the search box.
    if (searchIconAction_) {
        searchIconAction_->setToolTip(tr("Search (/)"));
    }
    searchEdit_->setToolTip(tr("Search mail — Gmail syntax. Press / to focus."));
    // Screen-reader name + hint for the search field. The leading icon
    // action inherits its description from the QLineEdit's accessible
    // name automatically.
    searchEdit_->setAccessibleName(tr("Search mail"));
    searchEdit_->setAccessibleDescription(tr(
        "Gmail search syntax — for example "
        "'from:alice subject:invoice has:attachment'. "
        "Press slash anywhere in the window to focus this field."));

    // Account dropdown — mirrors baremail's web UI: a single tool button
    // that pops a menu listing the signed-in email plus Sign out and
    // "Sign in with another account…". Single-account v1 means at most
    // one header row in the menu, but the structure mirrors what we'd
    // grow into for multi-account support later.
    accountButton_ = new QToolButton(tb);
    accountButton_->setIcon(IconLoader::themed(QStringLiteral("user.svg")));
    accountButton_->setPopupMode(QToolButton::InstantPopup);
    accountButton_->setToolButtonStyle(Preferences::toolbarShowText()
        ? Qt::ToolButtonTextBesideIcon
        : Qt::ToolButtonIconOnly);
    accountButton_->setAutoRaise(true);
    accountButton_->setCursor(Qt::PointingHandCursor);
    // Tooltip alone doesn't reach screen readers — set the accessible
    // name explicitly so Orca / Narrator / VoiceOver announce something
    // useful when this icon-only button takes focus. The dynamic
    // "Signed in as <email>" tooltip is also pushed to the accessible
    // description in refreshAccountMenu().
    accountButton_->setAccessibleName(tr("Account"));
    accountMenu_ = new QMenu(accountButton_);
    accountButton_->setMenu(accountMenu_);
    iconActions_.append({accountButton_->defaultAction(), QStringLiteral("user.svg")});
    auto* accountAction = tb->addWidget(accountButton_);
    overflowEntries_.push_back({accountAction, nullptr, tr("Accounts"), 8, {}, {}});
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
    overflowButton_->setAccessibleName(tr("More actions"));
    overflowMenu_ = new QMenu(overflowButton_);
    overflowButton_->setMenu(overflowMenu_);
    overflowAction_ = tb->addWidget(overflowButton_);
    overflowAction_->setVisible(false);

    // Capture each managed action's "anchor" — the next NON-overflow
    // action in the original toolbar layout. We deliberately skip over
    // other overflow entries: when updateToolbarOverflow tears the
    // managed actions out of the toolbar to recompute fit, the anchors
    // we use to reinsert them MUST still be present, otherwise
    // insertAction with a stale anchor falls back to appending at the
    // end and the toolbar ends up scrambled. Only the non-overflow
    // anchors (separators, the search bar, the hamburger button) are
    // guaranteed to survive a removal pass.
    {
        const auto current = tb->actions();
        QSet<QAction*> overflowSet;
        for (auto& e : overflowEntries_) overflowSet.insert(e.action);
        for (auto& e : overflowEntries_) {
            const int idx = current.indexOf(e.action);
            e.toolbarBefore = nullptr;
            for (int i = idx + 1; i < current.size(); ++i) {
                if (!overflowSet.contains(current[i])) {
                    e.toolbarBefore = current[i];
                    break;
                }
            }
        }
    }

    connect(refresh,    &QAction::triggered, this, &MainWindow::onRefresh);
    connect(compose,    &QAction::triggered, this, &MainWindow::onComposeNew);
    connect(reply,      &QAction::triggered, this, &MainWindow::onReplyCurrent);
    connect(replyAll,   &QAction::triggered, this, &MainWindow::onReplyAllCurrent);
    connect(forwardAct, &QAction::triggered, this, &MainWindow::onForwardCurrent);
    connect(archiveAct, &QAction::triggered, this, &MainWindow::onArchiveCurrent);
    connect(readAct,    &QAction::triggered, this, &MainWindow::onToggleReadCurrent);
    connect(snoozeAct,  &QAction::triggered, this, &MainWindow::onSnoozeCurrent);
    connect(trash,      &QAction::triggered, this, &MainWindow::onDeleteCurrent);
    connect(settings,   &QAction::triggered, this, &MainWindow::onOpenSettings);

    connect(searchEdit_, &QLineEdit::returnPressed, this, &MainWindow::onSearchSubmit);
    connect(searchEdit_, &QLineEdit::textChanged,   this, &MainWindow::onSearchChanged);
}

void MainWindow::onOpenSettings() {
    SettingsDialog dlg(this);
    // Settings → Storage → "Manage cache…" pops the cache-manager
    // dialog. SettingsDialog stays AccountManager-free; we host the
    // launch here where accounts_ is in scope.
    connect(&dlg, &SettingsDialog::cacheManagerRequested, this, [this] {
        CacheManagerDialog cdlg(accounts_, this);
        cdlg.exec();
    });
    dlg.exec();
    // Settings can flip Preferences::conversationView() (changes the
    // grouping in the message list), the toolbar layout, attachment
    // defaults, and label-colour display toggles; the theme listener
    // already lives on Theme::changed. Reload now so changes take
    // effect without needing the user to click a sidebar entry or
    // restart.
    refreshToolbarStyle();
    sidebar_->refreshAppearance();
    if (list_) list_->viewport()->update();
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

        // Pick the icon for the menu entry. menuIconName wins (so the
        // search entry can supply its own glyph; its toolbar widget is a
        // QLineEdit and has no icon to inherit from). Otherwise fall back
        // to the toolbar widget's own icon, then the action's icon.
        QIcon icon;
        if (!e->menuIconName.isEmpty()) {
            icon = IconLoader::themed(e->menuIconName);
        } else if (auto* w = qobject_cast<QToolButton*>(
                       toolBar_->widgetForAction(e->action))) {
            icon = w->icon();
        } else {
            icon = e->action->icon();
        }
        auto* proxy = overflowMenu_->addAction(icon, e->text);
        if (e->menuTrigger) {
            // Custom hamburger behaviour — used by the search overflow
            // entry to pop a small input dialog instead of doing nothing.
            auto cb = e->menuTrigger;
            connect(proxy, &QAction::triggered, this, [cb] { cb(); });
        } else {
            // Default: forward to the real action so the user gets the
            // same behaviour they'd get from clicking the toolbar button.
            QPointer<QAction> realAction(e->action);
            connect(proxy, &QAction::triggered, this, [realAction] {
                if (realAction) realAction->trigger();
            });
        }
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
    // Sync button keeps its own pre-rendered base pixmap for rotation —
    // refresh that too so a theme switch mid-session picks up the new
    // tint when the spin starts.
    if (syncBtn_) {
        syncBtn_->setBaseIcon(IconLoader::themed(QStringLiteral("refresh.svg")));
    }
}

void MainWindow::refreshToolbarStyle() {
    if (!toolBar_) return;
    const auto style = Preferences::toolbarShowText()
        ? Qt::ToolButtonTextBesideIcon
        : Qt::ToolButtonIconOnly;
    toolBar_->setToolButtonStyle(style);
    // QToolBar::setToolButtonStyle propagates to QToolButtons it owns,
    // but our two custom QToolButtons (Account, hamburger) live as
    // widgets — set them explicitly so the icon-only mode actually
    // hides their text.
    if (accountButton_)  accountButton_->setToolButtonStyle(style);
    // The hamburger button itself stays icon-only either way; nothing to do.
    updateToolbarOverflow();
}

void MainWindow::wireSignals() {
    connect(list_, &MessageListView::messageActivated,
            this,  &MainWindow::onMessageActivated);
    connect(list_, &MessageListView::starToggled,
            this,  &MainWindow::onToggleStarFor);

    connect(reader_, &ReaderPane::openAttachmentRequested,
            this,    &MainWindow::onOpenAttachment);
    connect(reader_, &ReaderPane::saveAsAttachmentRequested,
            this,    &MainWindow::onSaveAsAttachment);
    connect(reader_, &ReaderPane::downloadAllRequested,
            this,    &MainWindow::onDownloadAllAttachments);

    // Per-card Gmail-web-style action row in each message card. Each
    // signal carries the messageId of the SPECIFIC message the card
    // represents; handlers operate on that single message rather than
    // the whole thread (the toolbar buttons stay thread-scoped).
    connect(reader_, &ReaderPane::replyToMessageRequested,
            this,    &MainWindow::onReplyToMessage);
    connect(reader_, &ReaderPane::replyAllToMessageRequested,
            this,    &MainWindow::onReplyAllToMessage);
    connect(reader_, &ReaderPane::forwardMessageRequested,
            this,    &MainWindow::onForwardMessage);
    connect(reader_, &ReaderPane::archiveMessageRequested,
            this,    &MainWindow::onArchiveMessage);
    connect(reader_, &ReaderPane::markMessageReadRequested,
            this,    &MainWindow::onMarkMessageRead);
    connect(reader_, &ReaderPane::deleteMessageRequested,
            this,    &MainWindow::onDeleteMessage);
    connect(reader_, &ReaderPane::snoozeMessageRequested,
            this,    &MainWindow::onSnoozeMessage);
    // Hover a link in any message body → show its target in the
    // status bar; the existing messageChanged restorer puts the
    // signed-in-as / sync-progress baseline back when the cursor
    // leaves the link (we pass a 0-timeout temporary message which
    // clears as soon as we feed it an empty string).
    connect(reader_, &ReaderPane::urlHovered, this,
            [this](const QString& url) {
                if (url.isEmpty()) {
                    statusBar()->clearMessage();
                } else {
                    statusBar()->showMessage(url);
                }
            });

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
        // Start every per-account scheduler. New contexts that arrive
        // mid-session (Add account flow) are caught by the
        // accountsChanged hook below.
        for (auto* ctx : accounts_->allContexts()) {
            if (auto* s = ctx->sync()) {
                s->runOnce();
                s->startScheduler();
            }
        }
        if (accounts_->allContexts().isEmpty() && sync_) {
            sync_->runOnce();
            sync_->startScheduler();
        }
        outbox_->start();
        pending_->start();
        drafts_->start();
    });
    // Tokens load asynchronously from the keychain — refresh once they
    // arrive so the "Sign in" affordance flips to the signed-in account
    // menu without waiting for another refresh trigger.
    connect(auth_, &fc::auth::OAuthClient::tokensLoaded, this,
            &MainWindow::refreshAccountIndicator);
    // Re-evaluate the signed-in gate once keychain hydration finishes.
    // If this OAuthClient came back unauthorized, the UI must not
    // continue showing whatever cache-driven view was painted at
    // construction; if it came back authorized and we previously
    // wiped the UI, refresh it from cache for the now-active account.
    connect(auth_, &fc::auth::OAuthClient::tokensLoaded, this, [this] {
        enforceActiveAccountGate();
        if (auth_->isAuthorized() && !currentAccountId_.isEmpty()) {
            if (sidebar_ && sidebar_->model()
                    && sidebar_->model()->accountId() != currentAccountId_) {
                sidebar_->model()->setAccountId(currentAccountId_);
            }
            if (currentLabelId_.isEmpty()) {
                currentLabelId_ = QStringLiteral("INBOX");
            }
            reloadCurrentLabel();
        }
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
                // First sign-in for a fresh account: AccountManager has
                // no row for this email yet (the legacy seed has a
                // placeholder email or "legacy@local"). Add the row so
                // the toolbar account menu picks it up. Idempotent —
                // adds for an existing email return the same id and
                // just refresh display_name.
                if (accounts_) {
                    const QString id = accounts_->add(email);
                    if (!id.isEmpty()) {
                        accounts_->setCurrentAccountId(id);
                    }
                }
                refreshAccountIndicator();
            });
    // Per-account contexts that arrive mid-session (Add account flow)
    // need their schedulers kicked. accountsChanged fires on every
    // add()/remove(); piggy-back on the same hook to start any newly
    // created context.
    connect(accounts_, &fc::account::AccountManager::accountsChanged, this,
            [this] {
                if (!auth_->isAuthorized()) return;
                for (auto* ctx : accounts_->allContexts()) {
                    if (auto* s = ctx->sync()) {
                        // SyncService::startScheduler is idempotent —
                        // calling it on an already-running timer just
                        // resets the interval, which is fine.
                        s->startScheduler();
                    }
                }
            });
    connect(sync_, &fc::sync::SyncService::labelsUpdated,
            this,  &MainWindow::reloadSidebar);
    // Keep the in-memory label-style cache (used by the message-list
    // pill delegate) in sync with the freshly-synced labels table.
    // labelsUpdated runs in the UI thread (queued from the sync
    // thread), so invalidate() is safe to call here.
    // Lambda to disambiguate the now-overloaded LabelStyleCache::invalidate
    // (per-account vs zero-arg). We re-read the cache for whichever
    // account is currently active in MainWindow, so account switches
    // immediately surface their labels' colours in the message list.
    connect(sync_, &fc::sync::SyncService::labelsUpdated,
            this, [this] {
                LabelStyleCache::instance().invalidate(currentAccountId_);
            });
    // Aggregated per-account signals from AccountManager: any signed-in
    // account's sync ticks land here. We only repaint when the source
    // is the active account; other accounts' messagesUpdated still
    // tick the workers (Outbox / PendingOps / DraftSync) without
    // clobbering the UI.
    connect(accounts_, &fc::account::AccountManager::accountsChanged, this,
            &MainWindow::refreshAccountMenu);
    // Account switch: rebind currentAccountId_, retarget every model,
    // re-paint the sidebar, list, reader. The signed-in API stack
    // (auth_, gmail_, sync_) stays pointing at the previous account
    // for now — step 12 finishes the conversion to per-paint context
    // lookups. For sync that's already accurate (each AccountContext
    // owns its sync), so the switch immediately shows the new
    // account's cached data.
    connect(accounts_, &fc::account::AccountManager::currentAccountChanged,
            this, [this](const QString& aid) {
                currentAccountId_ = aid;
                if (sync_) sync_->setAccountId(aid);
                if (sidebar_ && sidebar_->model()) {
                    sidebar_->model()->setAccountId(aid);
                }
                LabelStyleCache::instance().invalidate(aid);
                listModel_->replaceAll({});
                reader_->showEmpty();
                currentMessage_ = {};
                currentRow_ = -1;
                currentLabelId_ = QStringLiteral("INBOX");
                reloadCurrentLabel();
                refreshAccountIndicator();
            });
    connect(accounts_, &fc::account::AccountManager::labelsUpdated, this,
            [this](const QString& aid) {
                if (aid != currentAccountId_) return;
                LabelStyleCache::instance().invalidate(currentAccountId_);
                reloadSidebar();
            });
    connect(accounts_, &fc::account::AccountManager::messagesUpdated, this,
            [this](const QString& aid) {
                if (aid == currentAccountId_) reloadCurrentLabel();
            });
    connect(accounts_, &fc::account::AccountManager::newMessages, this,
            [this](const QString& aid, int count) {
                // Per-account tray attribution: every signed-in account's
                // new-mail event triggers a toast tagged with the
                // account email so multi-account users can tell which
                // mailbox the toast belongs to. The privacy setting
                // (notification_mode = "preview" vs default
                // arrival-only) is per-account.
                onNewMessagesForAccount(aid, count);
            });
    connect(&LabelStyleCache::instance(), &LabelStyleCache::changed, this,
            [this] {
                if (list_) list_->viewport()->update();
            });
    // messagesUpdated covers two unrelated triggers: incremental sync
    // landing new rows, and our own topUpLabel finishing. Either way
    // we want to refresh in place — refreshFromSource re-queries the
    // same window the model already shows, so newly-cached rows
    // surface without dropping the user back to row 0. The scroll +
    // selection save/restore here covers the case where Qt's view
    // reacts to beginResetModel by snapping the scrollbar; setting
    // the value back after endResetModel keeps the user's pixel
    // window stable.
    connect(sync_, &fc::sync::SyncService::messagesUpdated,
            this,  [this] {
                auto* sb = list_->verticalScrollBar();
                const int y       = sb ? sb->value() : 0;
                const QString sel = currentMessage_.id;
                listModel_->refreshFromSource();
                if (sb) sb->setValue(y);
                if (!sel.isEmpty()) {
                    for (int i = 0; i < listModel_->rowCount(); ++i) {
                        const auto idx = listModel_->index(i, 0);
                        if (idx.data(fc::MessageListModel::IdRole).toString() == sel) {
                            list_->setCurrentIndex(idx);
                            break;
                        }
                    }
                }
                // Sidebar unread counts are aggregated from the
                // messages table, so they go stale every time a sync
                // upserts new rows. labelsUpdated alone reloads the
                // sidebar but fires BEFORE messages.list runs — the
                // counts that result reflect pre-sync state. Reload
                // again here so the (X) and (X…) in the sidebar
                // settle to the post-sync truth before the
                // stateChanged(Idle) handler shows "Done".
                reloadSidebar();
                refreshListFooter();
            });
    // When the model can't fetch any more rows from the cache, fall
    // through to the server-side top-up. Subsequent messagesUpdated
    // brings the new rows back via refreshFromSource above.
    connect(listModel_, &fc::MessageListModel::cacheExhausted,
            this,        [this](const QString& labelId) {
                if (sync_) sync_->topUpLabel(labelId);
                refreshListFooter();
            });

    // Label-scoped progress messages for top-up. Generic stateChanged
    // already shows "Syncing…" / "Syncing… Done" for INITIAL +
    // INCREMENTAL passes; these layer on top with a label name when
    // the top-up is for the visible label, so the user can tell
    // "Syncing Receipts…" apart from a global background pass.
    connect(sync_, &fc::sync::SyncService::topUpStarted, this,
            [this](const QString& labelId) {
                const QString name = fc::cache::LabelRepository::byId(labelId).name;
                statusBar()->showMessage(name.isEmpty()
                    ? tr("Syncing…")
                    : tr("Syncing %1…").arg(name));
                if (labelId == listModel_->sourceLabelId()) {
                    topUpInFlight_ = true;
                    refreshListFooter();
                }
            });
    connect(sync_, &fc::sync::SyncService::topUpFinished, this,
            [this](const QString& labelId, int newRows, bool serverExhausted) {
                const QString name = fc::cache::LabelRepository::byId(labelId).name;
                if (!name.isEmpty()) {
                    const QString msg = newRows > 0
                        ? tr("%1: %n new", "", newRows).arg(name)
                        : tr("%1: up to date").arg(name);
                    statusBar()->showMessage(msg, 30000);
                }
                serverExhaustedByLabel_[labelId] = serverExhausted;
                // The cache just gained `newRows` older rows. Push them
                // into the model so the user's scroll-to-bottom session
                // continues seamlessly. Skip if the user has navigated
                // away to a different label since the top-up started.
                if (newRows > 0
                    && labelId == listModel_->sourceLabelId()) {
                    listModel_->resumeAfterTopUp();
                }
                if (labelId == listModel_->sourceLabelId()) {
                    topUpInFlight_ = false;
                    refreshListFooter();
                }
            });
    connect(sync_, &fc::sync::SyncService::failed, this,
            [this](const QString& reason) {
                lastSyncFailed_ = true;
                statusBar()->showMessage(
                    tr("Sync failed: %1").arg(reason), 30000);
                // Persistent attention: keep a red chip in the status
                // bar AND fire a Critical tray toast. The transient
                // 30 s message in the slot above will roll off into
                // "Signed in as …" eventually, but the chip stays
                // until the user dismisses it or another sync starts.
                if (errorBanner_ && errorBannerLabel_) {
                    errorBannerLabel_->setText(reason);
                    errorBanner_->setToolTip(reason);
                    errorBanner_->show();
                }
                if (tray_ && tray_->notifier()) {
                    tray_->notifier()->notifyError(
                        tr("FirstContact — sync failed"), reason);
                }
            });
    // newMessages is now routed via AccountManager::newMessages above
    // (which re-emits the per-account context's signals). The legacy
    // single-instance sync_'s direct signal would double-fire under
    // multi-account, so we don't connect it here. The anonymous
    // fallback path (no contexts yet, fresh install) doesn't tick a
    // newMessages signal until the first profile fetch lands and the
    // AccountManager add() rebuilds the contexts.

    // Sync indicator on the main status-bar slot. SyncService emits
    // stateChanged(Idle) BEFORE emit failed (synchronously), so we
    // defer the success message via QTimer::singleShot(0); by the
    // time it runs the failed handler (if any) has set
    // lastSyncFailed_ and we skip the success path so the failure
    // message wins.
    connect(sync_, &fc::sync::SyncService::stateChanged, this,
            [this](fc::sync::SyncService::State s) {
                // Toolbar refresh icon: spin while a sync is active so
                // the user has a visual cue that we're talking to the
                // server. Stops on Idle.
                if (syncBtn_) {
                    syncBtn_->setSpinning(s != fc::sync::SyncService::State::Idle);
                }
                // Sidebar unread counts get an ellipsis hint while
                // sync is in flight, so the brief gap inside reload()
                // doesn't show up as flashing/empty parens.
                if (sidebar_ && sidebar_->model()) {
                    sidebar_->model()->setSyncing(
                        s != fc::sync::SyncService::State::Idle);
                }
                switch (s) {
                    case fc::sync::SyncService::State::InitialSync:
                        isSyncing_ = true;
                        if (errorBanner_) errorBanner_->hide();
                        statusBar()->showMessage(tr("Initial sync…"));
                        break;
                    case fc::sync::SyncService::State::IncrementalSync:
                        isSyncing_ = true;
                        if (errorBanner_) errorBanner_->hide();
                        statusBar()->showMessage(tr("Syncing…"));
                        break;
                    case fc::sync::SyncService::State::Idle: {
                        const bool wasSyncing = isSyncing_;
                        isSyncing_ = false;
                        if (!wasSyncing) break;   // no transition to mark
                        QTimer::singleShot(0, this, [this] {
                            if (lastSyncFailed_) {
                                lastSyncFailed_ = false;
                                return;          // failed handler wins
                            }
                            // Lingering "Done" so the user actually sees
                            // that sync completed; clears after 30 s and
                            // the messageChanged restorer below brings
                            // back the "Signed in as …" baseline.
                            statusBar()->showMessage(
                                tr("Syncing… Done"), 30000);
                        });
                        break;
                    }
                }
            });

    // Restore the baseline ("Signed in as …" or "Syncing…" if a sync
    // is mid-flight) every time a temporary message expires. Without
    // this, a quick "Archived." toast clearing during sync would
    // leave the bar empty until the next state change.
    connect(statusBar(), &QStatusBar::messageChanged, this,
            [this](const QString& text) {
                if (!text.isEmpty()) return;
                if (isSyncing_) {
                    statusBar()->showMessage(tr("Syncing…"));
                    return;
                }
                const QString email = auth_->accountEmail();
                statusBar()->showMessage(email.isEmpty()
                    ? tr("Not signed in.")
                    : tr("Signed in as %1").arg(email));
            });

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
    connect(shortcuts_, &Shortcuts::archiveAndPrev, this, &MainWindow::onArchiveAndPrev);
    connect(shortcuts_, &Shortcuts::archiveAndNext, this, &MainWindow::onArchiveAndNext);
    connect(shortcuts_, &Shortcuts::toggleStar,     this, &MainWindow::onToggleStar);
    connect(shortcuts_, &Shortcuts::markRead,       this, &MainWindow::onMarkReadCurrent);
    connect(shortcuts_, &Shortcuts::markUnread,     this, &MainWindow::onMarkUnreadCurrent);
    connect(shortcuts_, &Shortcuts::markImportant,    this, &MainWindow::onMarkImportant);
    connect(shortcuts_, &Shortcuts::markNotImportant, this, &MainWindow::onMarkNotImportant);
    connect(shortcuts_, &Shortcuts::snoozeCurrent,  this, &MainWindow::onSnoozeCurrent);
    connect(shortcuts_, &Shortcuts::muteThread,     this, &MainWindow::onMuteThread);
    connect(shortcuts_, &Shortcuts::reportSpam,     this, &MainWindow::onReportSpam);
    connect(shortcuts_, &Shortcuts::backToList,     this, &MainWindow::onBackToList);
    connect(shortcuts_, &Shortcuts::openCurrent,    this, &MainWindow::onOpenCurrent);
    connect(shortcuts_, &Shortcuts::goToLabel,        this, &MainWindow::onGoToLabel);
    connect(shortcuts_, &Shortcuts::toggleLinkDisplay, this, &MainWindow::onToggleLinkDisplay);
    connect(shortcuts_, &Shortcuts::applyLabels,    this, &MainWindow::onApplyLabelsCurrent);
    connect(shortcuts_, &Shortcuts::moveToLabel,    this, &MainWindow::onMoveToLabelCurrent);
    // selectNext/selectPrev should advance from whatever the user has
    // selected RIGHT NOW (which can be ahead of currentRow_ — e.g.
    // they clicked a row to select it but didn't activate it). Read
    // the live current index off the view rather than the cached
    // currentRow_ which only updates on activation.
    auto selectAt = [this](int row) {
        const int n = listModel_->rowCount();
        if (n == 0) return;
        row = qBound(0, row, n - 1);
        const auto idx = listModel_->index(row, 0);
        list_->setCurrentIndex(idx);
        onMessageActivated(idx.data(fc::MessageListModel::IdRole).toString(),
                            row);
    };
    auto selectedRow = [this] {
        const auto idx = list_->currentIndex();
        return idx.isValid() ? idx.row() : currentRow_;
    };
    connect(shortcuts_, &Shortcuts::selectNext, this, [selectAt, selectedRow] {
        selectAt(selectedRow() + 1);
    });
    connect(shortcuts_, &Shortcuts::selectPrev, this, [selectAt, selectedRow] {
        selectAt(selectedRow() - 1);
    });
    // Account switching via Ctrl+1..9 — multi-account only behaviour;
    // help dialog itself is wired via &MainWindow::onShowShortcutsHelp
    // elsewhere in this method.
    connect(shortcuts_, &Shortcuts::switchToAccountSlot, this,
            [this](int slot) {
                const auto list = accounts_->accounts();
                if (slot < 1 || slot > list.size()) return;
                accounts_->setCurrentAccountId(list.at(slot - 1).id);
            });
    connect(shortcuts_, &Shortcuts::showHelp, this, &MainWindow::onShowShortcutsHelp);
}

namespace {

QString humanBytes(qint64 b) {
    constexpr qint64 KB = 1024;
    constexpr qint64 MB = KB * 1024;
    constexpr qint64 GB = MB * 1024;
    if (b >= GB) return QStringLiteral("%1 GB").arg(b / double(GB), 0, 'f', 2);
    if (b >= MB) return QStringLiteral("%1 MB").arg(b / double(MB), 0, 'f', 1);
    if (b >= KB) return QStringLiteral("%1 KB").arg(b / double(KB), 0, 'f', 1);
    return QStringLiteral("%1 B").arg(b);
}

}  // namespace

void MainWindow::refreshBandwidthLabel() {
    if (!bandwidthLabel_) return;
    const auto& s = fc::api::SessionTransfer::instance();
    const qint64 down = s.bytesIn();
    const qint64 up   = s.bytesOut();
    const int reqs    = s.requestCount();
    bandwidthLabel_->setText(QStringLiteral("↓ %1").arg(humanBytes(down)));
    bandwidthLabel_->setAccessibleName(tr("Bandwidth used this session"));
    bandwidthLabel_->setToolTip(tr(
        "Session transfer since launch:\n"
        "↓ %1 received\n"
        "↑ %2 sent\n"
        "%3 request(s)")
        .arg(humanBytes(down), humanBytes(up))
        .arg(reqs));
}

void MainWindow::refreshAccountIndicator() {
    const QString email = auth_->accountEmail();
    const QString dryPrefix = fc::util::DryRun::enabled()
        ? QStringLiteral("[DRY RUN] ") : QString();
    setWindowTitle(dryPrefix + (email.isEmpty()
        ? QStringLiteral("FirstContact")
        : QStringLiteral("FirstContact — %1").arg(email)));
    // Don't stomp on a sync indicator that's currently in the slot —
    // the messageChanged restorer below picks up the new email next
    // time the slot frees up. Outside of an active sync, refresh the
    // baseline so the new email appears immediately.
    if (!isSyncing_) {
        statusBar()->showMessage(email.isEmpty()
            ? tr("Not signed in.")
            : tr("Signed in as %1").arg(email));
    }
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
    if (email.isEmpty()) {
        email = fc::cache::MetaRepository::get(currentAccountId_,
                                               QStringLiteral("email"));
    }
    const bool signedIn = auth_->isAuthorized();

    accountButton_->setText(tr("Accounts"));
    accountButton_->setToolTip(signedIn && !email.isEmpty()
        ? tr("Signed in as %1").arg(email)
        : tr("Manage Google accounts"));

    // One menu entry per *signed-in* account — i.e., accounts whose
    // per-account OAuthClient currently has valid tokens. The accounts
    // table can also hold rows for accounts that were signed in once
    // and signed out (sign-out keeps the row by default), plus a
    // synthetic "legacy@local" row that migration 0006 inserts on
    // every fresh install. Listing all of them in the toolbar would
    // surface a "legacy@local" entry on a launch with no real
    // accounts; filter to authorized ones here.
    //
    // The active account gets a checkmark; selecting another flips
    // currentAccountId via AccountManager::setCurrentAccountId, which
    // retargets sidebar / list / reader.
    //
    // v3: each entry's icon is a small filled circle in the account's
    // accent colour (or a transparent placeholder when no accent is
    // assigned, so the menu rows stay aligned).
    QList<fc::account::AccountInfo> allAccounts;
    for (const auto& a : accounts_->accounts()) {
        auto* ctx = accounts_->contextFor(a.id);
        if (ctx && ctx->auth() && ctx->auth()->isAuthorized()) {
            allAccounts.append(a);
        }
    }
    if (!allAccounts.isEmpty()) {
        for (const auto& a : allAccounts) {
            QString label = a.email.isEmpty() ? tr("Unknown account") : a.email;
            if (a.isDefault) label += tr(" (default)");
            QIcon chip;
            {
                QPixmap pm(16, 16);
                pm.fill(Qt::transparent);
                const QColor c = fc::account::AccountManager::accentColorFor(
                    a.colorHint);
                if (c.isValid()) {
                    QPainter p(&pm);
                    p.setRenderHint(QPainter::Antialiasing);
                    p.setBrush(c);
                    p.setPen(Qt::NoPen);
                    p.drawEllipse(2, 2, 12, 12);
                }
                chip = QIcon(pm);
            }
            auto* act = accountMenu_->addAction(chip, label);
            act->setCheckable(true);
            act->setChecked(a.id == currentAccountId_);
            const QString id = a.id;
            connect(act, &QAction::triggered, this, [this, id] {
                accounts_->setCurrentAccountId(id);
            });
        }
        accountMenu_->addSeparator();
    } else if (signedIn) {
        // Pre-step-6 fallback: anonymous stack, no accounts row. Show
        // the legacy header line.
        const QString headerText = email.isEmpty()
            ? tr("Unknown account")
            : email;
        auto* header = accountMenu_->addAction(headerText);
        header->setEnabled(false);
        accountMenu_->addSeparator();
    }

    // "Add another account…" — opens the OAuth flow on a fresh slot
    // without touching any existing account.
    auto* addAct = accountMenu_->addAction(
        IconLoader::themed(QStringLiteral("login.svg")),
        tr("Add another account…"));
    connect(addAct, &QAction::triggered, this, [this] {
        if (!config_->isConfigured()) {
            SetupWizard wiz(config_, this);
            if (wiz.exec() != QDialog::Accepted) return;
        }
        // The granted handler will mint the accounts row + start the
        // scheduler. We just kick the OAuth dance.
        statusBar()->showMessage(
            tr("Starting OAuth flow for new account — opening your browser…"),
            0);
        auth_->authorize();
    });

    // "Manage…" — full multi-row dialog (per-account sign-out,
    // make-default, etc.). Step 9 expands the dialog itself.
    auto* manageAct = accountMenu_->addAction(
        IconLoader::themed(QStringLiteral("user.svg")),
        tr("Manage…"));
    connect(manageAct, &QAction::triggered, this, [this] {
        AccountManagerDialog dlg(auth_, accounts_, this);
        connect(&dlg, &AccountManagerDialog::signOutRequested,
                this, &MainWindow::onSignOutAccount);
        connect(&dlg, &AccountManagerDialog::addAccountRequested,
                this, [this] {
                    // Add another → kick OAuth without signing out.
                    if (!config_->isConfigured()) {
                        SetupWizard wiz(config_, this);
                        if (wiz.exec() != QDialog::Accepted) return;
                    }
                    auth_->authorize();
                });
        dlg.exec();
    });

    // v4 — cache manager dialog. Lists every account's cached size +
    // orphan accounts; per-row drop / drop > N days actions.
    auto* cacheAct = accountMenu_->addAction(tr("Cache storage…"));
    connect(cacheAct, &QAction::triggered, this, [this] {
        CacheManagerDialog dlg(accounts_, this);
        dlg.exec();
    });
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

// Decides whether ANY account currently has valid OAuth tokens, and
// if not, clears the UI down to its signed-out state. Called at
// startup AND every time tokensLoaded fires across the per-account
// OAuthClients — the latter because keychain hydration is async, so
// the first call from the constructor may run before any tokens have
// landed. Once the hydration finishes:
//   - if at least one account is authorized, the UI is left alone
//     (or refreshed by the existing currentAccountChanged path).
//   - if no account is authorized, every cache-driven surface
//     (message list, sidebar tree, reader pane) is wiped so the
//     window stops rendering the previous account's data even
//     though that data still exists on disk.
//
// "Not signed in" is the strict condition. The accounts table can
// carry rows for sign-out-with-keep-cache and for the synthetic
// legacy seed; neither implies an active account.
void MainWindow::enforceActiveAccountGate() {
    bool anyAuthorized = false;
    if (accounts_) {
        for (auto* ctx : accounts_->allContexts()) {
            if (ctx && ctx->auth() && ctx->auth()->isAuthorized()) {
                anyAuthorized = true;
                break;
            }
        }
    }
    if (!anyAuthorized && auth_ && auth_->isAuthorized()) {
        anyAuthorized = true;
    }
    if (!anyAuthorized) {
        clearAccountUiState();
    }
}

// Resets every cache-driven UI surface (message list, sidebar tree,
// reader pane, error banner) so a window with no active account
// doesn't keep rendering the previous account's data straight from
// cache. Called on sign-out, on the last-account-removed transition,
// and at startup when the active accountId is empty.
void MainWindow::clearAccountUiState() {
    currentAccountId_.clear();
    currentMessage_ = {};
    currentRow_     = -1;
    currentLabelId_.clear();
    if (sync_)     sync_->setAccountId({});
    if (sidebar_ && sidebar_->model()) sidebar_->model()->setAccountId({});
    if (listModel_) listModel_->replaceAll({});
    if (reader_)    reader_->showEmpty(tr("Not signed in."));
    if (errorBanner_) errorBanner_->hide();
    if (outbox_)  outbox_->stop();
    if (pending_) pending_->stop();
    if (drafts_)  drafts_->stop();
    refreshAccountIndicator();
}

void MainWindow::onSignOut() {
    // Legacy single-account hook: routes to the per-account form for
    // the active account.
    onSignOutAccount(currentAccountId_);
}

void MainWindow::onSignOutAccount(const QString& accountId) {
    if (accountId.isEmpty()) return;

    // Pop the cache-disposition prompt: yes / no / cancel. yes
    // wipes the per-account cache rows (re-sign-in does a fresh
    // initial sync); no keeps them; cancel aborts.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Sign out"));
    const auto info = accounts_->accountById(accountId);
    box.setText(tr("Sign out of %1?")
                    .arg(info.email.isEmpty()
                         ? tr("this account") : info.email));
    box.setInformativeText(tr(
        "Drop the local cache for this account?\n\n"
        "Yes — wipe cached messages, drafts, outbox, and labels for this "
        "account. The next sign-in will do a full initial sync.\n\n"
        "No — keep the cache. The next sign-in resumes from where it "
        "left off, no re-download.\n\n"
        "Cancel — leave everything alone."));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No
                           | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::No);
    const int result = box.exec();
    if (result == QMessageBox::Cancel) return;
    const bool dropCache = result == QMessageBox::Yes;

    // Sign out the OAuth side. The OAuthClient currently aliases the
    // active account; signing out a non-active account via this path
    // is uncommon but possible (Manage… → Sign out a different row).
    // Step 12 will add per-account OAuth resolution here; for now,
    // sign out the active stack only when the active account matches.
    if (accountId == currentAccountId_) {
        auth_->signOut();
        // Reset transient session state so a stale "current message"
        // / "current row" can't drive a shortcut press (r, e, #, …)
        // into operating on cached data that no longer represents
        // the signed-in user. Reader pane goes back to its empty
        // hint. Per-account cache + scheduler cleanup happens below.
        currentMessage_ = {};
        currentRow_     = -1;
        if (reader_) reader_->showEmpty(tr("Not signed in."));
    } else if (auto* ctx = accounts_->contextFor(accountId)) {
        // Sign out the named context's OAuthClient directly.
        if (auto* a = ctx->auth()) a->signOut();
    }

    // Stop the named context's scheduler. The shared workers stay
    // running; their drain queries simply skip rows for the now-
    // signed-out account once its context is gone.
    if (auto* ctx = accounts_->contextFor(accountId)) {
        if (auto* s = ctx->sync()) s->stopScheduler();
    }

    if (dropCache) {
        accounts_->dropCache(accountId);
        statusBar()->showMessage(tr("Cache cleared for %1.")
                                     .arg(info.email), 5000);
    }

    // Clear the cached email under the named account so the next
    // sign-in's initial-sync re-populates it.
    fc::cache::MetaRepository::set(accountId,
                                   QStringLiteral("email"), QString());

    // If we just signed out the active account and another remains,
    // promote the most-recent. If none remain (last account signed
    // out), reset the panes.
    if (accountId == currentAccountId_) {
        QString next;
        for (const auto& a : accounts_->accounts()) {
            if (a.id == accountId) continue;
            // Pick the most-recently-used remaining account.
            if (next.isEmpty()) { next = a.id; continue; }
            // (We don't bother with full sort here — accounts() already
            // orders by sortOrder.)
        }
        if (!next.isEmpty()) {
            accounts_->setCurrentAccountId(next);
        } else {
            // Last account signed out — reset every UI surface so the
            // window stops rendering whichever account was active.
            clearAccountUiState();
        }
    }
    refreshAccountIndicator();
}

void MainWindow::onSwitchAccount() {
    // Multi-account: "Add another account" no longer signs out the
    // existing account — it just kicks the OAuth flow to mint a fresh
    // accounts row alongside the existing ones. The granted handler
    // builds the AccountContext and the toolbar account menu picks
    // up the new entry; the caller can switch to it via the menu or
    // Ctrl+N once it appears.
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
    if (id.isEmpty()) return;
    // v2 unified inbox: clicking "__all_inboxes" flips to cross-
    // account view. Any other label flips back to per-account.
    const bool wasCross = crossAccountView_;
    QString resolvedId;
    if (id == QStringLiteral("__all_inboxes")) {
        crossAccountView_ = true;
        resolvedId        = QStringLiteral("INBOX");   // implicit
    } else {
        crossAccountView_ = false;
        resolvedId        = id;
    }
    // Bail if neither the cross-account flip nor the underlying
    // label changed — repeated clicks on the same row should be a
    // no-op rather than a full reload.
    if (!wasCross && !crossAccountView_ && resolvedId == currentLabelId_) return;
    currentLabelId_ = resolvedId;
    // Reset top-up state for the new label — any in-flight top-up
    // for the old label is no longer interesting to the footer.
    topUpInFlight_ = false;
    refreshListFooter();
    reloadCurrentLabel();
    // Initial sync only seeds INBOX / SENT / DRAFT / STARRED — every
    // other label (every user label, plus categories like SPAM /
    // TRASH) only carries cached rows for messages that happened to
    // overlap with a seed at sync time. Pull a server-side page so
    // the user sees what Gmail web sees, not just the lucky overlap.
    // SyncService::topUpLabel itself is a no-op for the seed labels
    // and for the empty / search-mode case.
    if (sync_ && currentSearchQuery_.isEmpty()) {
        sync_->topUpLabel(id);
    }
    refreshListFooter();
}

void MainWindow::reloadSidebar() {
    sidebar_->model()->reload();
}

void MainWindow::refreshListFooter() {
    if (!listFooter_ || !listModel_) return;

    // While a server top-up for the visible label is in flight, the
    // footer always shows the loading state regardless of the
    // model's current row count — the user just triggered a fetch
    // and wants to know it's working.
    if (topUpInFlight_) {
        listFooter_->setText(tr("Loading more messages…"));
        listFooter_->setVisible(true);
        return;
    }

    const QString labelId = listModel_->sourceLabelId();
    if (labelId.isEmpty()) {
        listFooter_->setVisible(false);
        return;
    }

    // Cache still has more to give — don't show the footer; scrolling
    // pulls more rows in directly.
    if (!listModel_->cacheDrained()) {
        listFooter_->setVisible(false);
        return;
    }

    // Drained cache. Show "No more messages" if we know we've walked
    // the label end-to-end on the server, OR if this is one of the
    // seed labels that incremental sync keeps fully cached. For
    // non-seed labels where the server walk hasn't yielded an empty
    // nextPageToken yet, leave the footer hidden — a future scroll
    // will trigger another top-up that may bring more rows back.
    static const QSet<QString> seedLabels = {
        QStringLiteral("INBOX"),
        QStringLiteral("SENT"),
        QStringLiteral("DRAFT"),
        QStringLiteral("STARRED"),
    };
    const bool isSeed   = seedLabels.contains(labelId);
    const bool srvDone  = serverExhaustedByLabel_.value(labelId, false);

    if (isSeed || srvDone) {
        listFooter_->setText(tr("No more messages"));
        listFooter_->setVisible(true);
    } else {
        listFooter_->setVisible(false);
    }
}

void MainWindow::reloadCurrentLabel() {
    const bool conv = Preferences::conversationView();

    // Capture the user's currently-viewed message + thread so we can
    // re-select it after the model swap. Without this, every background
    // sync (which fires messagesUpdated → reloadCurrentLabel) would
    // wipe the selection and reset the reader pane — which is jarring
    // when the user is mid-read.
    const QString preservedId       = currentMessage_.id;
    const QString preservedThreadId = currentMessage_.threadId;

    if (crossAccountView_ && !currentSearchQuery_.isEmpty()) {
        // Cross-account search still uses replaceAll — searchFts is
        // top-K only, no offset support, so pagination doesn't apply.
        auto rows = conv
            ? fc::cache::MessageRepository::searchFtsThreadsAllAccounts(currentSearchQuery_, kPageSize)
            : fc::cache::MessageRepository::searchFtsAllAccounts(currentSearchQuery_, kPageSize);
        listModel_->replaceAll(std::move(rows));
    } else if (crossAccountView_) {
        // v2 unified inbox: cross-account label browsing now uses
        // the source-pinned cross-account mode so fetchMore walks
        // the *AllAccounts repository variants page-by-page just
        // like the per-account path. No top-up wiring is needed —
        // every contributing account's incremental sync keeps INBOX
        // (the only cross-account label today) fresh on its own.
        listModel_->setCrossAccountLabelSource(currentLabelId_, conv);
    } else if (currentSearchQuery_.isEmpty()) {
        // Single-account, label browsing: paginated source-pinned
        // mode. The legacy zero-arg listByLabel inside the model
        // routes through Database::defaultAccountId() — fine for
        // single-account-default; multi-account routing for the
        // paginated path lands in a follow-up.
        listModel_->setLabelSource(currentLabelId_, conv,
                                    Preferences::unreadOnly());
    } else {
        listModel_->setSearchSource(currentSearchQuery_, conv);
    }

    // Try to restore the selection: first by exact message id, then by
    // thread id. The thread fallback covers conversation-view rows
    // where a fresh message just landed and the row's id (== latest
    // message of the thread) has shifted to the new arrival.
    int restoredRow = -1;
    const int n = listModel_->rowCount();
    if (!preservedId.isEmpty()) {
        for (int i = 0; i < n; ++i) {
            const auto idx = listModel_->index(i, 0);
            if (idx.data(fc::MessageListModel::IdRole).toString() == preservedId) {
                restoredRow = i;
                break;
            }
        }
    }
    if (restoredRow < 0 && !preservedThreadId.isEmpty()) {
        for (int i = 0; i < n; ++i) {
            const auto idx = listModel_->index(i, 0);
            if (idx.data(fc::MessageListModel::ThreadIdRole).toString()
                    == preservedThreadId) {
                restoredRow = i;
                break;
            }
        }
    }
    if (restoredRow >= 0) {
        list_->setCurrentIndex(listModel_->index(restoredRow, 0));
        currentRow_ = restoredRow;
        // Reader keeps whatever it was showing — we deliberately don't
        // re-render the thread, even when the row's id shifted to a
        // newer message; that would be jarring while the user is
        // reading. The next explicit click triggers onMessageActivated
        // and pulls in the latest content.
    } else {
        currentRow_ = -1;
        reader_->showEmpty();
        currentMessage_ = {};
    }

    refreshListFooter();
}

void MainWindow::onMessageActivated(const QString& messageId, int row) {
    if (messageId.isEmpty()) return;
    currentRow_ = row;

    // v2: in cross-account mode, the row carries its source accountId.
    // Resolve the lookup against that account so we don't fall back to
    // currentAccountId_ (which is the toolbar selection, not the row's
    // owner).
    QString lookupAccount = currentAccountId_;
    if (crossAccountView_ && currentRow_ >= 0) {
        const auto idx = listModel_->index(currentRow_, 0);
        const auto rowAcc = idx.data(fc::MessageListModel::AccountIdRole).toString();
        if (!rowAcc.isEmpty()) lookupAccount = rowAcc;
    }
    fc::Message cached = fc::cache::MessageRepository::byId(lookupAccount,
                                                              messageId);

    auto renderThread = [this, lookupAccount](const fc::Message& selected) {
        currentMessage_ = selected;
        auto thread = fc::cache::MessageRepository::byThread(lookupAccount,
                                                              selected.threadId);
        if (thread.size() > 1) {
            // Pass the activated id so the reader expands + scrolls to
            // the right card. Without this, clicking a child row in an
            // expanded conversation always lands on the latest message
            // (showThread's default), which means the user has to
            // hunt for the message they actually clicked.
            reader_->showThread(thread, selected.id);
        } else {
            reader_->showMessage(selected);
        }
        fc::cache::MessageRepository::markAccessed(lookupAccount,
                                                    selected.id);

        // Auto-mark-as-read on open. Mirrors Gmail web — opening a
        // conversation marks every message in it as read on the
        // server. Drop UNREAD only when something is unread (avoids a
        // pointless server round-trip on already-read threads). Also
        // skipped under FC_DRY_RUN so debug sessions don't quietly
        // mutate state — applyLabelDiffToThread → enqueueModify is
        // already gated by PendingOpsWorker's dry-run check, but
        // bailing early skips the local cache write too.
        if (fc::util::DryRun::enabled()) return;
        bool anyUnread = false;
        for (const auto& m : thread) if (m.isUnread) { anyUnread = true; break; }
        if (anyUnread) {
            applyLabelDiffToThread(selected.threadId, {},
                                   {QStringLiteral("UNREAD")});
            currentMessage_.isUnread = false;
            // Repaint sidebar (unread counts) + message list (bold/
            // accent stripe drops away). reloadCurrentLabel is heavier
            // than needed but keeps the inbox row counts honest.
            reloadCurrentLabel();
            reloadSidebar();
        }
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
    const QString accountForFetch = currentAccountId_;
    gmail_->getMessage(messageId,
        [self, messageId, renderThread, accountForFetch]
        (fc::Message m, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                self->reader_->showEmpty(tr("Failed to load: %1").arg(err.message));
                return;
            }
            m.accountId = accountForFetch;
            fc::cache::MessageRepository::upsert(accountForFetch, m);
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
    const QString accountForSearch = currentAccountId_;
    gmail_->listMessages({}, q, {}, kPageSize,
        [self, gmail = gmail_, q, accountForSearch]
        (fc::api::GmailClient::ListPage page, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                self->statusBar()->showMessage(
                    tr("Server search failed: %1").arg(err.message));
                return;
            }
            // Hydrate any missing ids; the cache will then surface them.
            for (const auto& id : page.ids) {
                if (fc::cache::MessageRepository::exists(accountForSearch, id))
                    continue;
                gmail->getMessage(id,
                    [accountForSearch](fc::Message m, fc::api::ApiError gErr) {
                        if (!gErr) {
                            m.accountId = accountForSearch;
                            fc::cache::MessageRepository::upsert(
                                accountForSearch, m);
                        }
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

    // Build the From dropdown choices from every signed-in account.
    QList<ComposeWindow::AccountChoice> choices;
    for (const auto& a : accounts_->accounts()) {
        if (a.email.isEmpty()) continue;
        choices.append(ComposeWindow::AccountChoice{
            a.id, a.email, a.displayName});
    }
    if (choices.isEmpty()) {
        // Fallback for the anonymous-stack pre-sign-in path: synthesise
        // a single choice from auth_->accountEmail.
        const QString legacyEmail = auth_->accountEmail();
        if (!legacyEmail.isEmpty()) {
            choices.append(ComposeWindow::AccountChoice{
                currentAccountId_, legacyEmail, QString()});
        }
    }

    // Default selection rule:
    //   1. Reply / Reply-all / Forward → the parent message's accountId
    //      (so the reply goes from the account that received the mail).
    //   2. New compose → most-recently-sent-from. Each send stamps
    //      last_used_from = email under the sender account; we pick the
    //      account whose row has the most recent value.
    //   3. Falls back to the active sidebar account.
    QString defaultId;
    if (parent && !parent->accountId.isEmpty()
        && static_cast<ComposeWindow::Mode>(mode) != ComposeWindow::Mode::New) {
        defaultId = parent->accountId;
    } else {
        // most-recently-sent-from. We don't store a timestamp; we rely
        // on the simple rule "the most recent setter wins" — overwriting
        // last_used_from always touches account_meta(updated_at) under
        // the hood. v1 keeps it simpler: pick the account whose
        // last_used_from is non-empty AND whose accounts.last_used_at is
        // most recent (already a stamp we maintain via markUsed). If no
        // last_used_from has ever been set, fall through to the active
        // account.
        qint64 bestStamp = -1;
        for (const auto& a : accounts_->accounts()) {
            const QString hint = fc::cache::MetaRepository::get(
                a.id, QStringLiteral("last_used_from"));
            if (hint.isEmpty()) continue;
            if (a.lastUsedAt > bestStamp) {
                bestStamp = a.lastUsedAt;
                defaultId = a.id;
            }
        }
        if (defaultId.isEmpty()) defaultId = currentAccountId_;
    }

    auto* w = new ComposeWindow(choices, defaultId, this);
    w->setAttribute(Qt::WA_DeleteOnClose);
    // Always prefill — Mode::New seeds the signature too, so calling it
    // for a brand-new compose isn't an empty no-op.
    const fc::Message empty;
    w->prefillFrom(parent ? *parent : empty,
                    static_cast<ComposeWindow::Mode>(mode));
    connect(w, &ComposeWindow::composeReady, this,
        [this](const QString& accountId,
               const fc::util::OutgoingMessage& msg, const QString& threadId,
               qint64 sendAtMs) {
            const QString sendAccount = accountId.isEmpty()
                ? currentAccountId_ : accountId;
            const QByteArray rfc = fc::util::MimeBuilder::build(msg);
            fc::cache::OutboxItem item;
            item.accountId = sendAccount;
            item.rfc5322   = rfc;
            item.threadId  = threadId;
            item.sendAt    = sendAtMs;   // 0 → send immediately
            fc::cache::OutboxRepository::enqueue(sendAccount, item);
            // Stamp last_used_from on the sending account so the next
            // compose defaults back to it.
            if (!sendAccount.isEmpty()) {
                fc::cache::MetaRepository::set(sendAccount,
                    QStringLiteral("last_used_from"), msg.fromAddr);
                accounts_->markUsed(sendAccount);
            }
            // Immediate send: poke the worker to flush right now.
            // Scheduled send: the worker's existing timer will catch
            // the row when it becomes due.
            if (sendAtMs == 0) outbox_->flush();
            else {
                statusBar()->showMessage(
                    tr("Scheduled to send at %1.")
                        .arg(QDateTime::fromMSecsSinceEpoch(sendAtMs)
                                 .toString(QStringLiteral("ddd MMM d, h:mm AP"))),
                    8000);
            }
        });
    connect(w, &ComposeWindow::saveDraftRequested, this,
        [this, w](const QString& accountId,
                  const fc::util::OutgoingMessage& msg, const QString& threadId,
                  const QString& existingDraftId) {
            const QString draftAccount = accountId.isEmpty()
                ? currentAccountId_ : accountId;
            fc::cache::DraftRow row;
            row.accountId          = draftAccount;
            row.id                 = existingDraftId;
            row.threadId           = threadId;
            row.subject            = msg.subject;
            row.toAddrs            = msg.to;
            row.ccAddrs            = msg.cc;
            row.bccAddrs           = msg.bcc;
            row.bodyText           = msg.bodyText;
            row.inReplyToMessageId = msg.rfc822InReplyTo;
            row.dirty              = true;
            const QString id = fc::cache::DraftRepository::upsert(draftAccount, row);
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

// ---- Per-card (per-message) handlers ----------------------------------
//
// The card's reply button + 3-dot overflow menu fire signals that route
// here. Unlike on*Current, these target the SPECIFIC messageId carried
// by the signal — Gmail web's per-card semantics. Reply / Reply-all /
// Forward open compose with that exact message as the parent (instead
// of the focused message). Archive / Mark-read / Delete / Snooze apply
// the label diff to ONE message instead of looping across the whole
// thread.

void MainWindow::onReplyToMessage(const QString& messageId) {
    if (messageId.isEmpty()) return;
    fc::Message m = fc::cache::MessageRepository::byId(messageId);
    if (m.id.isEmpty()) return;
    openComposeWindow(&m, int(ComposeWindow::Mode::Reply));
}

void MainWindow::onReplyAllToMessage(const QString& messageId) {
    if (messageId.isEmpty()) return;
    fc::Message m = fc::cache::MessageRepository::byId(messageId);
    if (m.id.isEmpty()) return;
    openComposeWindow(&m, int(ComposeWindow::Mode::ReplyAll));
}

void MainWindow::onForwardMessage(const QString& messageId) {
    if (messageId.isEmpty()) return;
    fc::Message m = fc::cache::MessageRepository::byId(messageId);
    if (m.id.isEmpty()) return;
    openComposeWindow(&m, int(ComposeWindow::Mode::Forward));
}

void MainWindow::onArchiveMessage(const QString& messageId) {
    if (messageId.isEmpty()) return;
    if (fc::util::DryRun::block(QStringLiteral("archive-message-card"))) {
        statusBar()->showMessage(
            tr("Dry-run mode: archive blocked."), 4000);
        return;
    }
    const QStringList rem{QStringLiteral("INBOX")};
    fc::cache::MessageRepository::applyLabelDiff(messageId, {}, rem);
    fc::cache::PendingOpsRepository::enqueueModify(messageId, {}, rem);
    pending_->flush();
    statusBar()->showMessage(tr("Message archived."), 3000);
    reloadCurrentLabel();
}

void MainWindow::onMarkMessageRead(const QString& messageId, bool read) {
    if (messageId.isEmpty()) return;
    if (fc::util::DryRun::block(QStringLiteral("mark-read-card"))) {
        statusBar()->showMessage(
            tr("Dry-run mode: mark read/unread blocked."), 4000);
        return;
    }
    QStringList add, rem;
    if (read) rem << QStringLiteral("UNREAD");
    else      add << QStringLiteral("UNREAD");
    fc::cache::MessageRepository::applyLabelDiff(messageId, add, rem);
    fc::cache::PendingOpsRepository::enqueueModify(messageId, add, rem);
    pending_->flush();
    statusBar()->showMessage(read ? tr("Marked read.") : tr("Marked unread."),
                              3000);
    reloadCurrentLabel();
}

void MainWindow::onDeleteMessage(const QString& messageId) {
    if (messageId.isEmpty()) return;
    if (fc::util::DryRun::block(QStringLiteral("delete-message-card"))) {
        statusBar()->showMessage(
            tr("Dry-run mode: move-to-Trash blocked."), 4000);
        return;
    }
    if (QMessageBox::question(this, tr("Delete this message"),
            tr("Move just this message to Trash? "
               "Other messages in the conversation are unaffected."),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    const QStringList add{QStringLiteral("TRASH")};
    const QStringList rem{QStringLiteral("INBOX")};
    fc::cache::MessageRepository::applyLabelDiff(messageId, add, rem);
    fc::cache::PendingOpsRepository::enqueueModify(messageId, add, rem);
    pending_->flush();
    statusBar()->showMessage(tr("Message moved to Trash."), 3000);
    reloadCurrentLabel();
}

void MainWindow::onSnoozeMessage(const QString& messageId) {
    if (messageId.isEmpty()) return;
    if (fc::util::DryRun::block(QStringLiteral("snooze-message-card"))) {
        statusBar()->showMessage(tr("Dry-run mode: snooze blocked."), 4000);
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Snooze message"));
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(tr(
        "Pick a wake-up time. This message drops out of Inbox now and "
        "reappears at the chosen time. Other messages in the same "
        "thread are unaffected."), &dlg));
    auto* picker = new QDateTimeEdit(
        QDateTime::currentDateTime().addSecs(60 * 60 * 3), &dlg);
    picker->setCalendarPopup(true);
    picker->setMinimumDateTime(QDateTime::currentDateTime().addSecs(60));
    picker->setDisplayFormat(QStringLiteral("ddd MMM d, yyyy  h:mm AP"));
    layout->addWidget(picker);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* okBtn     = new QPushButton(tr("Snooze"), &dlg);
    okBtn->setObjectName(QStringLiteral("primary"));
    okBtn->setDefault(true);
    auto* cancelBtn = new QPushButton(tr("Cancel"), &dlg);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(okBtn);
    layout->addLayout(btnRow);
    QObject::connect(okBtn,     &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    const QDateTime when = picker->dateTime();
    if (when <= QDateTime::currentDateTime()) {
        statusBar()->showMessage(tr("Pick a time in the future."), 4000);
        return;
    }

    const qint64 wakeAt = when.toMSecsSinceEpoch();
    fc::cache::MessageRepository::setSnoozeUntil(messageId, wakeAt);
    const QStringList rem{QStringLiteral("INBOX")};
    fc::cache::MessageRepository::applyLabelDiff(messageId, {}, rem);
    fc::cache::PendingOpsRepository::enqueueModify(messageId, {}, rem);
    pending_->flush();
    statusBar()->showMessage(
        tr("Snoozed until %1.").arg(when.toString(QStringLiteral("MMM d, h:mm AP"))),
        4000);
    reloadCurrentLabel();
}

void MainWindow::onCreateLabel(const QString& parentLabelId) {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New label"),
        tr("Label name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.isEmpty()) return;

    const auto parent = fc::cache::LabelRepository::byId(currentAccountId_,
                                                         parentLabelId);
    if (!parent.id.isEmpty() && parent.type == QLatin1String("user")) {
        name = parent.name + QLatin1Char('/') + name;
    }
    QPointer<MainWindow> self(this);
    const QString accountForCreate = currentAccountId_;
    gmail_->createLabel(name,
        [self, accountForCreate]
        (fc::api::GmailClient::Label l, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                QMessageBox::warning(self, tr("Create label"), err.message);
                return;
            }
            fc::cache::LabelRow row;
            row.accountId = accountForCreate;
            row.id        = l.id;
            row.name      = l.name;
            row.type      = l.type;
            fc::cache::LabelRepository::upsert(accountForCreate, row);
            self->reloadSidebar();
        });
}

void MainWindow::onRenameLabel(const QString& labelId) {
    if (fc::util::DryRun::block(QStringLiteral("rename-label"))) {
        statusBar()->showMessage(
            tr("Dry-run mode: label rename blocked."), 4000);
        return;
    }
    const auto current = fc::cache::LabelRepository::byId(currentAccountId_,
                                                          labelId);
    if (current.id.isEmpty() || current.type != QLatin1String("user")) return;

    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("Rename label"),
        tr("New name:"), QLineEdit::Normal, current.name, &ok);
    if (!ok || newName.isEmpty() || newName == current.name) return;

    QPointer<MainWindow> self(this);
    const QString accountForRename = currentAccountId_;
    gmail_->updateLabel(labelId, newName,
        [self, labelId, newName, accountForRename]
        (fc::api::GmailClient::Label, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                QMessageBox::warning(self, tr("Rename label"), err.message);
                return;
            }
            auto row = fc::cache::LabelRepository::byId(accountForRename, labelId);
            row.name = newName;
            fc::cache::LabelRepository::upsert(accountForRename, row);
            self->reloadSidebar();
        });
}

void MainWindow::onDeleteLabel(const QString& labelId) {
    if (fc::util::DryRun::block(QStringLiteral("delete-label"))) {
        statusBar()->showMessage(
            tr("Dry-run mode: label deletion blocked."), 4000);
        return;
    }
    const auto current = fc::cache::LabelRepository::byId(currentAccountId_,
                                                          labelId);
    if (current.id.isEmpty() || current.type != QLatin1String("user")) return;
    if (QMessageBox::question(this, tr("Delete label"),
            tr("Delete the label '%1'? Messages keep their other labels.")
              .arg(current.name)) != QMessageBox::Yes) return;

    QPointer<MainWindow> self(this);
    const QString accountForDelete = currentAccountId_;
    gmail_->deleteLabel(labelId,
        [self, labelId, accountForDelete](fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                QMessageBox::warning(self, tr("Delete label"), err.message);
                return;
            }
            fc::cache::LabelRepository::remove(accountForDelete, labelId);
            self->reloadSidebar();
        });
}

void MainWindow::onToggleStar() {
    if (currentMessage_.id.isEmpty()) return;
    onToggleStarFor(currentMessage_.id);
}

void MainWindow::onToggleStarFor(const QString& messageId) {
    if (messageId.isEmpty()) return;
    if (fc::util::DryRun::block(QStringLiteral("toggle-star"))) {
        statusBar()->showMessage(
            tr("Dry-run mode: star toggle blocked."), 4000);
        return;
    }

    // Re-read from cache so we toggle relative to the row the user clicked,
    // not to whatever currentMessage_ happens to be (the click target may
    // not be the currently-selected row).
    const fc::Message m = fc::cache::MessageRepository::byId(currentAccountId_,
                                                              messageId);
    if (m.id.isEmpty()) return;

    QStringList add, rem;
    if (m.isStarred) rem << QStringLiteral("STARRED");
    else             add << QStringLiteral("STARRED");

    fc::cache::MessageRepository::applyLabelDiff(currentAccountId_, messageId,
                                                  add, rem);
    fc::cache::PendingOpsRepository::enqueueModify(currentAccountId_, messageId,
                                                    add, rem);
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

namespace {

// Per-process scratch directory for "Open" attachments. Lazily created the
// first time the user clicks an attachment and reused for the rest of the
// session so the OS keeps file→app associations stable. Cleaned up when
// the QTemporaryDir destructor runs (process exit) — see
// MainWindow::~MainWindow if we ever add explicit teardown.
QString sessionTempDir() {
    static QString cached;
    if (!cached.isEmpty()) return cached;
    QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    const QString dir = base + QStringLiteral("/firstcontact-")
                      + QString::number(QCoreApplication::applicationPid());
    QDir().mkpath(dir);
    cached = dir;
    return cached;
}

}  // namespace

void MainWindow::onOpenAttachment(const QString& messageId,
                                  const QString& attachmentId,
                                  const QString& filename) {
    if (messageId.isEmpty() || attachmentId.isEmpty()) {
        statusBar()->showMessage(tr("Attachment is not downloadable."), 5000);
        return;
    }
    if (!auth_->isAuthorized()) {
        statusBar()->showMessage(
            tr("Sign in to open attachments."), 5000);
        return;
    }

    statusBar()->showMessage(tr("Opening %1…").arg(filename));
    QPointer<MainWindow> self(this);
    gmail_->getAttachment(messageId, attachmentId,
        [self, filename](QByteArray bytes, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                self->statusBar()->showMessage(
                    tr("Open failed: %1").arg(err.message), 8000);
                return;
            }
            // Drop into a per-session temp dir so multiple "Open" clicks
            // for the same filename don't collide and the OS can hold
            // the file open as long as the viewer wants to.
            const QString target = uniqueTargetPath(sessionTempDir(), filename);
            QString writeErr;
            if (!writeBytesToPath(target, bytes, &writeErr)) {
                self->statusBar()->showMessage(
                    tr("Couldn't write temp file: %1").arg(writeErr), 8000);
                return;
            }
            // Intentionally NOT calling AttachmentRepository::markDownloaded —
            // these temp files aren't a permanent download.
            self->statusBar()->showMessage(
                tr("Opened %1").arg(QFileInfo(target).fileName()), 5000);
            QDesktopServices::openUrl(QUrl::fromLocalFile(target));
        });
}

void MainWindow::onSaveAsAttachment(const QString& messageId,
                                    const QString& attachmentId,
                                    const QString& filename) {
    if (messageId.isEmpty() || attachmentId.isEmpty()) {
        statusBar()->showMessage(tr("Attachment is not downloadable."), 5000);
        return;
    }
    if (!auth_->isAuthorized()) {
        statusBar()->showMessage(
            tr("Sign in to download attachments."), 5000);
        return;
    }

    // Resolve the destination synchronously so the picker is a direct
    // response to the user's gesture; showing it from inside the
    // network callback would feel disconnected.
    const QString suggested = Preferences::attachmentDir()
        + QLatin1Char('/')
        + (filename.isEmpty() ? QStringLiteral("attachment.bin")
                              : QFileInfo(filename).fileName());
    const QString target = QFileDialog::getSaveFileName(
        this, tr("Save attachment as"), suggested);
    if (target.isEmpty()) {
        statusBar()->showMessage(tr("Save cancelled."), 3000);
        return;
    }

    statusBar()->showMessage(tr("Downloading %1…").arg(filename));
    QPointer<MainWindow> self(this);
    const QString accountForSave = currentAccountId_;
    gmail_->getAttachment(messageId, attachmentId,
        [self, attachmentId, target, accountForSave]
        (QByteArray bytes, fc::api::ApiError err) {
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
            fc::cache::AttachmentRepository::markDownloaded(accountForSave,
                                                            attachmentId,
                                                            target);
            // No auto-open here — the user explicitly chose Save as…, so
            // we respect that and don't pop a viewer afterwards.
            self->statusBar()->showMessage(tr("Saved to %1").arg(target), 8000);
        });
}

void MainWindow::onDownloadAllAttachments(const QString& messageId) {
    if (messageId.isEmpty()) return;
    if (!auth_->isAuthorized()) {
        statusBar()->showMessage(
            tr("Sign in to download attachments."), 5000);
        return;
    }

    const auto m = fc::cache::MessageRepository::byId(currentAccountId_,
                                                       messageId);
    if (m.attachments.empty()) {
        statusBar()->showMessage(tr("No attachments on this message."), 4000);
        return;
    }

    // For "Download all" the natural unit is a folder, not a per-file
    // picker (asking N times in a row is hostile UX). The picker starts
    // in the user's configured default folder. Each file gets
    // uniqueTargetPath collision handling within that folder.
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Save all attachments to folder"),
        Preferences::attachmentDir());
    if (dir.isEmpty()) {
        statusBar()->showMessage(tr("Download cancelled."), 3000);
        return;
    }
    QDir().mkpath(dir);

    int kicked = 0;
    QPointer<MainWindow> self(this);
    const QString accountForBatch = currentAccountId_;
    for (const auto& a : m.attachments) {
        if (a.id.isEmpty()) continue;   // inline-only; not addressable
        const QString target = uniqueTargetPath(dir, a.filename);
        const QString attachmentId = a.id;
        const QString filename     = a.filename;
        gmail_->getAttachment(messageId, attachmentId,
            [self, attachmentId, target, filename, accountForBatch]
            (QByteArray bytes, fc::api::ApiError err) {
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
                fc::cache::AttachmentRepository::markDownloaded(accountForBatch,
                                                                attachmentId,
                                                                target);
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
    if (fc::util::DryRun::block(QStringLiteral("delete-message"))) {
        statusBar()->showMessage(
            tr("Dry-run mode: move-to-Trash blocked."), 4000);
        return;
    }
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
    fc::cache::MessageRepository::applyLabelDiff(currentAccountId_, id, add, rem);
    fc::cache::PendingOpsRepository::enqueueModify(currentAccountId_, id, add, rem);
    pending_->flush();
    statusBar()->showMessage(tr("Moved to Trash."), 3000);
    reloadCurrentLabel();
}

void MainWindow::applyLabelDiffToThread(const QString& threadId,
                                         const QStringList& add,
                                         const QStringList& remove) {
    if (threadId.isEmpty()) return;
    const auto messages = fc::cache::MessageRepository::byThread(currentAccountId_,
                                                                  threadId);
    for (const auto& m : messages) {
        if (m.id.isEmpty()) continue;
        fc::cache::MessageRepository::applyLabelDiff(currentAccountId_, m.id,
                                                      add, remove);
        fc::cache::PendingOpsRepository::enqueueModify(currentAccountId_, m.id,
                                                        add, remove);
    }
    pending_->flush();
}

bool MainWindow::guardedThreadAction(const QString& dryRunKey,
                                      const QString& blockedStatus,
                                      const QString& successStatus,
                                      const QStringList& add,
                                      const QStringList& remove,
                                      bool refreshSidebar) {
    if (currentMessage_.id.isEmpty()) return false;
    if (fc::util::DryRun::block(dryRunKey)) {
        statusBar()->showMessage(blockedStatus, 4000);
        return false;
    }
    applyLabelDiffToThread(currentMessage_.threadId, add, remove);
    statusBar()->showMessage(successStatus, 3000);
    reloadCurrentLabel();
    if (refreshSidebar) reloadSidebar();
    return true;
}

void MainWindow::onArchiveCurrent() {
    // Archive the entire conversation, matching Gmail web semantics.
    // For single-message rows, byThread inside applyLabelDiffToThread
    // returns the one message and the loop is a no-op extra cost.
    guardedThreadAction(
        QStringLiteral("archive-message"),
        tr("Dry-run mode: archive blocked."),
        tr("Archived."),
        /*add=*/{},
        /*remove=*/{QStringLiteral("INBOX")});
}

void MainWindow::onToggleReadCurrent() {
    if (currentMessage_.id.isEmpty()) return;
    if (fc::util::DryRun::block(QStringLiteral("toggle-read"))) {
        statusBar()->showMessage(
            tr("Dry-run mode: read-toggle blocked."), 4000);
        return;
    }

    // Read state for the row in the message list reflects "any message
    // in the thread is unread"; mirror that here. If anything is unread
    // we treat the whole thread as currently-unread and mark it read,
    // and vice versa. Avoids the surprise of "I clicked toggle and only
    // one of three messages flipped state."
    const auto messages = fc::cache::MessageRepository::byThread(
                              currentAccountId_, currentMessage_.threadId);
    bool anyUnread = false;
    for (const auto& m : messages) if (m.isUnread) { anyUnread = true; break; }

    if (anyUnread) onMarkReadCurrent();
    else           onMarkUnreadCurrent();
}

void MainWindow::onMarkReadCurrent() {
    if (guardedThreadAction(
            QStringLiteral("mark-read"),
            tr("Dry-run mode: mark-read blocked."),
            tr("Marked as read."),
            /*add=*/{},
            /*remove=*/{QStringLiteral("UNREAD")},
            /*refreshSidebar=*/true)) {
        currentMessage_.isUnread = false;
    }
}

void MainWindow::onMarkUnreadCurrent() {
    if (guardedThreadAction(
            QStringLiteral("mark-unread"),
            tr("Dry-run mode: mark-unread blocked."),
            tr("Marked as unread."),
            /*add=*/{QStringLiteral("UNREAD")},
            /*remove=*/{},
            /*refreshSidebar=*/true)) {
        currentMessage_.isUnread = true;
    }
}

void MainWindow::onBackToList() {
    // Gmail-web `u`: yank focus from the reader pane back to the
    // threadlist. Doesn't change selection — pressing j/k from there
    // continues to navigate from where you were.
    if (!list_) return;
    list_->setFocus(Qt::ShortcutFocusReason);
    if (currentRow_ >= 0 && currentRow_ < listModel_->rowCount()) {
        list_->setCurrentIndex(listModel_->index(currentRow_, 0));
    }
}

void MainWindow::onOpenCurrent() {
    // Gmail-web `o` / Enter: re-trigger the activation pipeline for
    // whichever row is selected. If nothing's selected (fresh load,
    // empty inbox), no-op.
    if (!list_ || !listModel_) return;
    const auto idx = list_->currentIndex();
    if (!idx.isValid()) return;
    const QString id = idx.data(fc::MessageListModel::IdRole).toString();
    if (id.isEmpty()) return;
    onMessageActivated(id, idx.row());
    // Reader pane gets focus so PageDown/PageUp scroll the message
    // body — matches Gmail-web's behavior where `o` "opens" the thread.
    if (reader_) reader_->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::onArchiveAndPrev() {
    if (currentMessage_.id.isEmpty()) return;
    const int prevRow = currentRow_;
    onArchiveCurrent();
    // After reload, the row we want is at prevRow - 1 (the message that
    // was directly above the one we just archived). Clamp to [0, n-1].
    const int n = listModel_ ? listModel_->rowCount() : 0;
    if (n == 0) return;
    const int target = qMax(prevRow - 1, 0);
    if (target >= n) return;
    list_->setCurrentIndex(listModel_->index(target, 0));
    onMessageActivated(listModel_->index(target, 0)
                          .data(fc::MessageListModel::IdRole).toString(), target);
}

void MainWindow::onArchiveAndNext() {
    if (currentMessage_.id.isEmpty()) return;
    const int prevRow = currentRow_;
    onArchiveCurrent();
    // After reload, what was at prevRow + 1 is now at prevRow (the row
    // we archived collapsed out of the list). Clamp to [0, n-1].
    const int n = listModel_ ? listModel_->rowCount() : 0;
    if (n == 0) return;
    const int target = qMin(prevRow, n - 1);
    if (target < 0) return;
    list_->setCurrentIndex(listModel_->index(target, 0));
    onMessageActivated(listModel_->index(target, 0)
                          .data(fc::MessageListModel::IdRole).toString(), target);
}

void MainWindow::onMuteThread() {
    // Apply MUTE + drop INBOX — same data shape as Gmail web's mute
    // action. We don't auto-archive future incoming replies (Gmail's
    // server does that for you on its end), but the label is correct
    // and reconciles cleanly when the server side reflects back.
    guardedThreadAction(
        QStringLiteral("mute-thread"),
        tr("Dry-run mode: mute blocked."),
        tr("Muted."),
        /*add=*/{QStringLiteral("MUTE")},
        /*remove=*/{QStringLiteral("INBOX")});
}

void MainWindow::onReportSpam() {
    guardedThreadAction(
        QStringLiteral("report-spam"),
        tr("Dry-run mode: spam blocked."),
        tr("Reported as spam."),
        /*add=*/{QStringLiteral("SPAM")},
        /*remove=*/{QStringLiteral("INBOX")});
}

void MainWindow::onMarkImportant() {
    if (currentMessage_.id.isEmpty()) return;
    if (fc::util::DryRun::block(QStringLiteral("mark-important"))) {
        statusBar()->showMessage(tr("Dry-run mode: mark-important blocked."), 4000);
        return;
    }
    applyLabelDiffToThread(currentMessage_.threadId,
                            {QStringLiteral("IMPORTANT")}, {});
    statusBar()->showMessage(tr("Marked important."), 3000);
}

void MainWindow::onMarkNotImportant() {
    if (currentMessage_.id.isEmpty()) return;
    if (fc::util::DryRun::block(QStringLiteral("mark-not-important"))) {
        statusBar()->showMessage(tr("Dry-run mode: mark-not-important blocked."), 4000);
        return;
    }
    applyLabelDiffToThread(currentMessage_.threadId,
                            {}, {QStringLiteral("IMPORTANT")});
    statusBar()->showMessage(tr("Marked not important."), 3000);
}

void MainWindow::onGoToLabel(const QString& labelId) {
    if (!sidebar_) return;
    sidebar_->selectLabel(labelId);
}

void MainWindow::onToggleLinkDisplay() {
    using fc::util::LinkDisplayMode;
    const auto current = Preferences::linkDisplayMode();
    const auto next    = (current == LinkDisplayMode::Labeled)
        ? LinkDisplayMode::FullUrl
        : LinkDisplayMode::Labeled;
    Preferences::setLinkDisplayMode(next);
    statusBar()->showMessage(next == LinkDisplayMode::FullUrl
        ? tr("Showing full URLs")
        : tr("Showing link labels"), 3000);

    // Re-render the reader so the change is immediately visible.
    if (currentMessage_.id.isEmpty()) return;
    const auto cached = fc::cache::MessageRepository::byId(currentMessage_.id);
    if (cached.id.isEmpty()) return;
    const auto thread = fc::cache::MessageRepository::byThread(cached.threadId);
    if (thread.size() > 1) {
        reader_->showThread(thread, cached.id);
    } else {
        reader_->showMessage(cached);
    }
}

void MainWindow::onApplyLabelsCurrent() {
    if (currentMessage_.id.isEmpty()) return;
    if (fc::util::DryRun::block(QStringLiteral("apply-labels"))) {
        statusBar()->showMessage(tr("Dry-run mode: label edit blocked."), 4000);
        return;
    }

    // Union of label ids across every message in the thread — that's
    // the set the dialog should pre-check. Using a thread-level union
    // matches what the user sees in the message-list row badges and
    // avoids the surprise of "I checked Work but only one of three
    // messages had it; now the others are missing it."
    const auto messages = fc::cache::MessageRepository::byThread(
                              currentMessage_.threadId);
    QSet<QString> applied;
    for (const auto& m : messages) {
        for (const auto& id : m.labelIds) applied.insert(id);
    }

    LabelChooserDialog dlg(LabelChooserDialog::Mode::Apply, applied, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const QStringList add = dlg.added();
    const QStringList rem = dlg.removed();
    if (add.isEmpty() && rem.isEmpty()) return;

    applyLabelDiffToThread(currentMessage_.threadId, add, rem);
    statusBar()->showMessage(tr("Labels updated."), 3000);
    reloadCurrentLabel();
    reloadSidebar();
}

void MainWindow::onMoveToLabelCurrent() {
    if (currentMessage_.id.isEmpty()) return;
    if (fc::util::DryRun::block(QStringLiteral("move-to-label"))) {
        statusBar()->showMessage(tr("Dry-run mode: move blocked."), 4000);
        return;
    }

    const auto messages = fc::cache::MessageRepository::byThread(
                              currentMessage_.threadId);
    QSet<QString> applied;
    for (const auto& m : messages) {
        for (const auto& id : m.labelIds) applied.insert(id);
    }

    LabelChooserDialog dlg(LabelChooserDialog::Mode::MoveTo, applied, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString chosen = dlg.chosen();
    if (chosen.isEmpty()) return;  // nothing picked

    // "Move" semantics: add the chosen label, drop the active sidebar
    // label. The sidebar label is what the user is currently looking
    // at (e.g. INBOX, or some user label). For system pseudo-labels
    // ("STARRED", "SENT", "DRAFT") moving doesn't really make sense
    // — the action there degrades gracefully to a plain "add label",
    // which is the right thing to do.
    QStringList rem;
    if (!currentLabelId_.isEmpty() && currentLabelId_ != chosen) {
        rem << currentLabelId_;
    }
    QStringList add;
    if (!applied.contains(chosen)) add << chosen;

    if (add.isEmpty() && rem.isEmpty()) return;
    applyLabelDiffToThread(currentMessage_.threadId, add, rem);
    statusBar()->showMessage(tr("Moved."), 3000);
    reloadCurrentLabel();
    reloadSidebar();
}

void MainWindow::onShowShortcutsHelp() {
    // Grouped reference modeled on Gmail-web's own "?" overlay.
    // We render via a QDialog with a QGridLayout so the shortcut keys
    // stay right-aligned in their own column and the descriptions stay
    // left-aligned in another, regardless of translation length.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Keyboard shortcuts"));
    dlg.setModal(true);

    auto* outer = new QVBoxLayout(&dlg);

    struct Group { QString title; QList<QPair<QString, QString>> rows; };
    const QList<Group> groups = {
        { tr("Navigation"), {
            { QStringLiteral("/"),       tr("Focus search") },
            { QStringLiteral("j"),       tr("Next message") },
            { QStringLiteral("k"),       tr("Previous message") },
            { tr("o or Enter"),          tr("Open conversation") },
            { QStringLiteral("u"),       tr("Back to threadlist") },
        }},
        { tr("Go to"), {
            { QStringLiteral("g i"),     tr("Inbox") },
            { QStringLiteral("g s"),     tr("Starred") },
            { QStringLiteral("g t"),     tr("Sent") },
            { QStringLiteral("g d"),     tr("Drafts") },
        }},
        { tr("Compose"), {
            { QStringLiteral("c"),       tr("New message") },
            { QStringLiteral("r"),       tr("Reply") },
            { tr("a or Shift+R"),        tr("Reply all") },
            { QStringLiteral("f"),       tr("Forward") },
            { QStringLiteral("Ctrl+Enter"), tr("Send (compose window)") },
            { QStringLiteral("Esc"),     tr("Close compose window") },
        }},
        { tr("Read state"), {
            { QStringLiteral("Shift+I"), tr("Mark as read") },
            { QStringLiteral("Shift+U"), tr("Mark as unread") },
            { QStringLiteral("s"),       tr("Toggle star") },
            { QStringLiteral("="),       tr("Mark important") },
            { QStringLiteral("-"),       tr("Mark not important") },
        }},
        { tr("Organize"), {
            { QStringLiteral("e"),       tr("Archive conversation") },
            { QStringLiteral("["),       tr("Archive + previous") },
            { QStringLiteral("]"),       tr("Archive + next") },
            { QStringLiteral("#"),       tr("Delete conversation") },
            { QStringLiteral("b"),       tr("Snooze conversation") },
            { QStringLiteral("m"),       tr("Mute thread") },
            { QStringLiteral("!"),       tr("Report as spam") },
            { QStringLiteral("l"),       tr("Apply labels…") },
            { QStringLiteral("v"),       tr("Move to label…") },
        }},
        { tr("View"), {
            { QStringLiteral("Shift+L"), tr("Toggle link display (label only ↔ label + URL)") },
        }},
        { tr("Help"), {
            { QStringLiteral("?"),       tr("Show this dialog") },
        }},
    };

    for (const Group& g : groups) {
        auto* heading = new QLabel(QStringLiteral("<b>%1</b>").arg(g.title), &dlg);
        outer->addWidget(heading);

        auto* grid = new QGridLayout;
        grid->setContentsMargins(16, 0, 0, 0);
        grid->setHorizontalSpacing(18);
        grid->setVerticalSpacing(4);

        int row = 0;
        for (const auto& kv : g.rows) {
            auto* keyLabel = new QLabel(QStringLiteral("<code>%1</code>").arg(kv.first), &dlg);
            keyLabel->setTextFormat(Qt::RichText);
            keyLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            keyLabel->setMinimumWidth(110);
            grid->addWidget(keyLabel, row, 0);

            auto* descLabel = new QLabel(kv.second, &dlg);
            descLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            grid->addWidget(descLabel, row, 1);
            ++row;
        }
        outer->addLayout(grid);
        outer->addSpacing(6);
    }

    auto* closeBtn = new QPushButton(tr("Close"), &dlg);
    closeBtn->setDefault(true);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn);
    outer->addLayout(btnRow);
    QObject::connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.exec();
}

void MainWindow::onSnoozeCurrent() {
    if (currentMessage_.id.isEmpty()) return;
    if (fc::util::DryRun::block(QStringLiteral("snooze-thread"))) {
        statusBar()->showMessage(tr("Dry-run mode: snooze blocked."), 4000);
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Snooze conversation"));
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(tr(
        "Pick a wake-up time. The conversation drops out of Inbox now "
        "and reappears at the chosen time. Snooze is FirstContact-local: "
        "Gmail's API doesn't expose its native snooze, so the wake-up "
        "fires only when FirstContact is running."), &dlg));
    auto* picker = new QDateTimeEdit(
        QDateTime::currentDateTime().addSecs(60 * 60 * 3), &dlg);
    picker->setCalendarPopup(true);
    picker->setMinimumDateTime(QDateTime::currentDateTime().addSecs(60));
    picker->setDisplayFormat(QStringLiteral("ddd MMM d, yyyy  h:mm AP"));
    layout->addWidget(picker);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* okBtn     = new QPushButton(tr("Snooze"), &dlg);
    okBtn->setObjectName(QStringLiteral("primary"));
    okBtn->setDefault(true);
    auto* cancelBtn = new QPushButton(tr("Cancel"), &dlg);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(okBtn);
    layout->addLayout(btnRow);
    QObject::connect(okBtn,     &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    const QDateTime when = picker->dateTime();
    if (when <= QDateTime::currentDateTime()) {
        statusBar()->showMessage(tr("Pick a time in the future."), 4000);
        return;
    }

    // Drop INBOX from every message in the thread + stamp snooze_until
    // so the wake-up timer can find them when the time comes. The
    // INBOX drop also propagates to Gmail web (good — the user sees
    // the conversation gone from Inbox there too); the wake-up will
    // re-apply INBOX which restores it on Gmail web's side as well.
    const QString tid = currentMessage_.threadId;
    const auto thread = fc::cache::MessageRepository::byThread(currentAccountId_,
                                                                tid);
    const qint64 wakeAt = when.toMSecsSinceEpoch();
    for (const auto& m : thread) {
        if (m.id.isEmpty()) continue;
        fc::cache::MessageRepository::setSnoozeUntil(currentAccountId_,
                                                      m.id, wakeAt);
    }
    applyLabelDiffToThread(tid, {}, {QStringLiteral("INBOX")});
    statusBar()->showMessage(
        tr("Snoozed until %1.")
            .arg(when.toString(QStringLiteral("ddd MMM d, h:mm AP"))),
        5000);
    reloadCurrentLabel();
    reloadSidebar();
}

void MainWindow::wakeDueSnoozedMessages() {
    if (fc::util::DryRun::enabled()) return;
    if (currentAccountId_.isEmpty()) return;
    const QStringList due = fc::cache::MessageRepository::dueSnoozeWakeups(
                                currentAccountId_);
    if (due.isEmpty()) return;
    QSet<QString> threads;
    for (const auto& mid : due) {
        const auto m = fc::cache::MessageRepository::byId(currentAccountId_, mid);
        if (!m.threadId.isEmpty()) threads.insert(m.threadId);
        // Clear snooze_until first so a same-tick failure doesn't
        // produce an infinite wake loop.
        fc::cache::MessageRepository::setSnoozeUntil(currentAccountId_, mid, 0);
    }
    // Restore INBOX per thread (one diff per thread, not per message).
    for (const QString& tid : threads) {
        applyLabelDiffToThread(tid, {QStringLiteral("INBOX")}, {});
    }
    qInfo("Snooze: woke %d message(s) across %d thread(s)",
          int(due.size()), int(threads.size()));
    reloadCurrentLabel();
    reloadSidebar();
}

void MainWindow::onNewMessages(int count) {
    // Legacy single-account hook: forward to the per-account form
    // pinned to the current account.
    onNewMessagesForAccount(currentAccountId_, count);
}

void MainWindow::onNewMessagesForAccount(const QString& accountId, int count) {
    if (count <= 0 || !tray_ || !tray_->notifier()) return;

    // Pull the most recent message we just upserted from the source
    // account so the toast reflects it correctly even when the user is
    // looking at a different account.
    auto recent = fc::cache::MessageRepository::listByLabel(
        accountId, QStringLiteral("INBOX"), 1, 0);
    QString sender, subject, threadId;
    if (!recent.empty()) {
        sender   = recent.front().fromName.isEmpty()
                     ? recent.front().fromAddr
                     : recent.front().fromName;
        subject  = recent.front().subject;
        threadId = recent.front().threadId;
    }

    const QString accountEmail = fc::cache::MetaRepository::get(
        accountId, QStringLiteral("email"));

    // Per-account notification preference. v1 default is ArrivalOnly
    // (privacy + multi-account hygiene); the user can flip to Preview
    // via the per-account preferences (settings dialog work — see
    // Preferences). We read the cache row directly so the setting
    // doesn't need an in-memory cache.
    const QString modeStr = fc::cache::MetaRepository::get(
        accountId, QStringLiteral("notification_mode"));
    Notifier::NewMailMode mode = (modeStr == QStringLiteral("preview"))
        ? Notifier::NewMailMode::Preview
        : Notifier::NewMailMode::ArrivalOnly;

    tray_->notifier()->notifyNewMail(mode, accountEmail, count,
                                      sender, subject, threadId);
}

}  // namespace fc::ui
