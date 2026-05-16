#include "MainWindow.h"

#include "account/AccountContext.h"
#include "account/AccountManager.h"
#include "api/GmailClient.h"
#include "api/RestClient.h"
#include "api/SessionTransfer.h"
#include "auth/ClientConfig.h"
#include "auth/OAuthClient.h"
#include "cache/AttachmentRepository.h"
#include "cache/BodyCompressionWorker.h"
#include "cache/Database.h"
#include "cache/DraftRepository.h"
#include "cache/LabelRepository.h"
#include "cache/MessageRepository.h"
#include "cache/MetaRepository.h"
#include "cache/OutboxRepository.h"
#include "cache/PendingOpsRepository.h"
#include "common/AccountManagerDialog.h"
#include "common/CacheManagerDialog.h"
#include "common/CompressionProgressDialog.h"
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
#include "util/Format.h"
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
#include <QMetaObject>
#include <QTimer>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QStyle>
#include <QPushButton>
#include <QSize>
#include <QMenu>
#include <QResizeEvent>
#include <QScopedValueRollback>
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

// AccountContext's per-account stack (OAuthClient / RestClient /
// GmailClient / SyncService) currently lives on the UI thread with the
// context. These thin wrappers keep deferred calls readable and avoid
// repeating the lambda+Qt::QueuedConnection boilerplate at call sites
// that expect async completion.
template<class Target, class Fn>
inline void postToObject(Target* target, Fn&& fn) {
    if (!target) return;
    QMetaObject::invokeMethod(target, std::forward<Fn>(fn),
                               Qt::QueuedConnection);
}
}  // namespace

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

    // Seed the active account from AccountManager's selection.
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

    // Construction-time gate. Per-context keychain hydration is
    // async — at this point every context's OAuthClient.isAuthorized()
    // returns false even when the slot is about to land valid tokens
    // a millisecond later. Calling enforceActiveAccountGate() here
    // would clobber currentAccountId_ on a fresh launch with a real
    // signed-in account, and the eventual tokensLoaded relay can't
    // restore it cleanly because AccountManager::setCurrentAccountId
    // early-returns when the id is unchanged — leaving MainWindow's
    // view of "no current account" and AccountManager's view ("yes,
    // 39592a3a-…") permanently out of sync.
    //
    // Instead we only fire the gate synchronously when we can be
    // certain there is no account at all (fresh install, all rows
    // signed-out-and-removed). Otherwise we trust the tokensLoaded
    // relay to drive the UI transition once hydration settles.
    if (accounts_ && accounts_->accounts().isEmpty()) {
        clearAccountUiState();
    }
    // Helper: any per-account context already holding tokens? After
    // the Add-account refactor, auth_ is the Bootstrap-anonymous
    // OAuthClient and never authorizes; the right signal is whether
    // any AccountContext has hydrated successfully. Hydration is
    // async, so this may answer "no" at construction time even when
    // a real account is on disk — the per-context tokensLoaded
    // re-emit through AccountManager picks up the slack later.
    auto anyContextAuthorized = [this] {
        if (!accounts_) return false;
        for (auto* c : accounts_->allContexts()) {
            if (c && c->auth() && c->auth()->isAuthorized()) return true;
        }
        return false;
    };
    if (anyContextAuthorized()) {
        reloadSidebar();
        restoreStartupSelection();
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

    // If we have credentials already, kick off background sync. Each
    // account runs its own SyncService timer so a stuck account
    // doesn't starve others; the workers stay shared because their
    // drain queries run cross-account and dispatch via the
    // GmailResolver.
    //
    // We start an authorized context's scheduler eagerly here AND
    // again from the per-context tokensLoaded relay below — keychain
    // hydration is async, so contexts that aren't authorized at
    // construction time may flip to authorized milliseconds later.
    bool anyStarted = false;
    for (auto* ctx : accounts_->allContexts()) {
        if (!ctx || !ctx->auth() || !ctx->auth()->isAuthorized()) continue;
        if (auto* s = ctx->sync()) {
            // Queue the calls through SyncService's event loop so its
            // state changes and QTimer setup happen after the current
            // UI/account bootstrap pass unwinds.
            postToObject(s, [s] { s->runOnce(); s->startScheduler(); });
            anyStarted = true;
        }
    }
    if (anyStarted) {
        outbox_->start();
        pending_->start();
        drafts_->start();
    }
    applyBackgroundCrawlerSettings();
}

void MainWindow::applyBackgroundCrawlerSettings() {
    if (!accounts_) return;
    const bool enabled = Preferences::backgroundCrawl();
    const int interval = Preferences::backgroundCrawlIntervalSec();
    for (auto* ctx : accounts_->allContexts()) {
        if (!ctx) continue;
        if (auto* s = ctx->sync()) {
            // SyncService::configureBackgroundCrawl owns a QTimer. Use
            // the standard postToObject dispatch so repeated settings
            // changes settle in event-loop order.
            postToObject(s, [s, enabled, interval] {
                s->configureBackgroundCrawl(enabled, interval);
            });
        }
    }
}

void MainWindow::buildLayout() {
    splitter_ = new QSplitter(Qt::Horizontal, this);

    sidebar_ = new SidebarWidget(splitter_);
    sidebar_->setMaximumWidth(260);
    // Wire the sidebar to AccountManager so it can build the multi-
    // account tree (one branch per signed-in account, "All Inboxes"
    // at the top). It listens to accountsChanged + currentAccountChanged
    // and pushes the live list into LabelTreeModel::setAccounts.
    if (accounts_) sidebar_->setAccountManager(accounts_);
    // Also mark the active account as the "focus" so the tree's
    // syncing-ellipsis hint targets the right branch from launch.
    if (sidebar_->model() && !currentAccountId_.isEmpty()) {
        sidebar_->model()->setAccountId(currentAccountId_);
    }

    // Wrap the list view + a small footer label in a single column so
    // the footer ("Loading more messages…" / "No more messages") sits
    // tight beneath the list within the splitter cell.
    auto* listColumn = new QWidget(splitter_);
    auto* listColumnLayout = new QVBoxLayout(listColumn);
    listColumnLayout->setContentsMargins(0, 0, 0, 0);
    listColumnLayout->setSpacing(0);

    list_      = new MessageListView(listColumn);
    listModel_ = new fc::MessageListModel(this);
    listModel_->setAccountId(currentAccountId_);
    list_->setModel(listModel_);
    listColumnLayout->addWidget(list_, /*stretch=*/1);

    // Footer is now an in-list synthetic row (driven by
    // MessageListModel::setFooterState) rather than a separate
    // widget. The model appends a "Loading more messages…" /
    // "No more messages" placeholder row that the delegate renders
    // as centered italic text and that scrolls naturally with the
    // rest of the list.
    listFooter_ = nullptr;

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
    connect(&dlg, &SettingsDialog::recompressRequested, this, [this] {
        onRecompressAllAccounts();
    });
    connect(&dlg, &SettingsDialog::backgroundCrawlSettingsChanged, this,
            [this] { applyBackgroundCrawlerSettings(); });
    connect(&dlg, &SettingsDialog::backgroundCrawlResetRequested, this,
            [this] {
                if (!accounts_) return;
                for (auto* ctx : accounts_->allContexts()) {
                    if (!ctx) continue;
                    if (auto* s = ctx->sync()) {
                        postToObject(s, [s] {
                            s->resetBackgroundCrawlProgress();
                        });
                    }
                }
                statusBar()->showMessage(
                    tr("Background prefetch progress reset for every account."),
                    4000);
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
    connect(sidebar_, &SidebarWidget::requestCacheLabel,
            this,     &MainWindow::onCacheLabel);
    connect(accounts_, &fc::account::AccountManager::cacheLabelProgress,
            this, [this](const QString& aid, const QString& labelId, int total) {
                if (aid != currentAccountId_) return;
                const QString name = fc::cache::LabelRepository::byId(
                    aid, labelId).name;
                statusBar()->showMessage(
                    tr("Caching %1: %n message(s) so far…", "", total)
                      .arg(name.isEmpty() ? labelId : name),
                    10000);
            });
    connect(accounts_, &fc::account::AccountManager::cacheLabelFinished,
            this, [this](const QString& aid, const QString& labelId,
                          int total, bool cancelled) {
                if (aid != currentAccountId_) return;
                const QString name = fc::cache::LabelRepository::byId(
                    aid, labelId).name;
                const QString label = name.isEmpty() ? labelId : name;
                if (cancelled) {
                    statusBar()->showMessage(
                        tr("Caching %1 cancelled after %n message(s).", "",
                            total).arg(label), 15000);
                } else {
                    statusBar()->showMessage(
                        tr("Cached %1 end-to-end: %n new message(s) added.",
                            "", total).arg(label), 15000);
                }
            });

    connect(auth_, &fc::auth::OAuthClient::granted, this, [this] {
        refreshAccountIndicator();
        // Status-bar confirmation that OAuth completed. The signed-
        // in-as baseline that refreshAccountIndicator restores is
        // the persistent indicator; this transient toast is the
        // immediate "yes, the OAuth flow finished" signal.
        statusBar()->showMessage(tr("Sign-in successful."), 6000);
        // Start every per-account scheduler. New contexts that arrive
        // mid-session (Add account flow) are caught by the
        // accountsChanged hook below.
        for (auto* ctx : accounts_->allContexts()) {
            if (auto* s = ctx->sync()) {
                postToObject(s, [s] { s->runOnce(); s->startScheduler(); });
            }
        }
        if (accounts_->allContexts().isEmpty() && sync_) {
            // Legacy anonymous sync_ stays on the UI thread; direct
            // calls are still safe here.
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
            [this](const QUrl& url, bool /*openedAutomatically*/) {
                // We deliberately don't auto-launch; OAuthClient::authorize
                // emits this signal but no longer fires util::launchBrowser
                // itself. The dialog gives the user explicit Copy / Open in
                // Browser controls and waits for them to choose.
                QApplication::clipboard()->setText(url.toString());

                auto* dlg = new QDialog(this);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                dlg->setWindowTitle(tr("Sign in with Google"));
                dlg->resize(640, 240);

                auto* layout = new QVBoxLayout(dlg);
                const QString headline = tr(
                    "<p>To finish signing in, open the URL below in your "
                    "browser. <b>Click <i>Open in Browser</i></b>, or "
                    "copy the URL and paste it into any browser yourself."
                    "</p>");
                const QString footnote = tr(
                    "<p>The URL is already on your clipboard. FirstContact is "
                    "listening on a local port — once you complete consent, "
                    "the redirect will land here and this dialog will switch "
                    "to a sign-in-complete confirmation.</p>");

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

                // When the redirect lands, swap the dialog into a
                // "sign-in complete" state instead of auto-closing.
                // The user has to click Done — earlier behaviour
                // (auto-close on grant) gave no visible confirmation
                // that OAuth had finished, so users who completed the
                // browser dance came back to a window that just looked
                // like nothing had happened.
                QPointer<QDialog>     dlgPtr(dlg);
                QPointer<QLabel>      msgPtr(msg);
                QPointer<QLineEdit>   urlPtr(urlField);
                QPointer<QPushButton> copyPtr(copyBtn);
                QPointer<QPushButton> retryPtr(retryBtn);
                QPointer<QPushButton> closePtr(closeBtn);
                auto showSuccess = [this, dlgPtr, msgPtr, urlPtr,
                                     copyPtr, retryPtr, closePtr] {
                    if (!dlgPtr || !msgPtr) return;
                    const QString email = auth_ ? auth_->accountEmail()
                                                : QString();
                    const QString text = email.isEmpty()
                        ? tr("<h3 style='margin:0'>Signed in</h3>"
                             "<p>OAuth completed successfully. "
                             "We're fetching account details and starting "
                             "the first inbox sync now.</p>"
                             "<p>You can close this dialog.</p>")
                        : tr("<h3 style='margin:0'>Signed in as %1</h3>"
                             "<p>OAuth completed successfully. "
                             "Inbox sync is starting now.</p>"
                             "<p>You can close this dialog.</p>")
                              .arg(email.toHtmlEscaped());
                    msgPtr->setText(text);
                    if (urlPtr)   urlPtr->hide();
                    if (copyPtr)  copyPtr->hide();
                    if (retryPtr) retryPtr->hide();
                    if (closePtr) {
                        closePtr->setText(tr("Done"));
                        closePtr->setObjectName(QStringLiteral("primary"));
                        closePtr->setDefault(true);
                        // Re-polish so the "primary" object-name styling
                        // (background colour, etc.) takes effect after
                        // the live name change.
                        closePtr->style()->unpolish(closePtr);
                        closePtr->style()->polish(closePtr);
                        closePtr->setFocus();
                    }
                    if (dlgPtr) {
                        dlgPtr->setWindowTitle(
                            tr("Sign in with Google — done"));
                    }
                };
                connect(auth_, &fc::auth::OAuthClient::granted,
                        dlg,  showSuccess);
                // Email isn't known until the first profileFetched
                // (Gmail's getProfile runs after token exchange). If
                // the dialog already flipped to success on `granted`,
                // refresh the headline once the email lands.
                connect(sync_, &fc::sync::SyncService::profileFetched,
                        dlg, [showSuccess](const QString&) { showSuccess(); });
                connect(auth_, &fc::auth::OAuthClient::failed,
                        dlg,  &QDialog::close);

                dlg->show();
            });

    connect(sync_, &fc::sync::SyncService::profileFetched, this,
            [this](const QString& email) {
                auth_->setAccountEmail(email);
                // First sign-in for a fresh account: AccountManager has
                // no row for this email yet. Add the row so the toolbar
                // account menu picks it up. Idempotent — adds for an
                // existing email return the same id and just refresh
                // display_name.
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
    // add()/remove(); start the scheduler on every authorized
    // context. SyncService::startScheduler is idempotent — calling
    // it on an already-running timer just resets the interval, which
    // is fine.
    connect(accounts_, &fc::account::AccountManager::accountsChanged, this,
            [this] {
                for (auto* ctx : accounts_->allContexts()) {
                    if (!ctx || !ctx->auth() || !ctx->auth()->isAuthorized())
                        continue;
                    if (auto* s = ctx->sync()) {
                        postToObject(s, [s] { s->startScheduler(); });
                    }
                }
            });
    // Per-context tokensLoaded relay. Keychain hydration on app
    // restart is async; this fires once a context has finished
    // loading its slot. We re-evaluate the active-account gate, kick
    // the per-account sync that just became authorized, and
    // refresh the indicator so the toolbar shows the right email.
    connect(accounts_, &fc::account::AccountManager::cacheCleared, this,
            [this](const QString& accountId) {
                // Cache wipe leaves the in-memory views (sidebar tree,
                // message list) showing rows that no longer exist —
                // they were populated from cache before the wipe and
                // nothing has yet emitted messagesUpdated/labelsUpdated
                // to refresh them. Reload now so the user sees an
                // empty mailbox immediately. If the wiped account is
                // the active one, also kick a runOnce so we re-pull
                // from Gmail without waiting for the 60s scheduler
                // tick — and schedule a 1500ms retry, because the
                // SyncService may still be busy with the in-flight
                // tick that ran just before the cache wipe (which
                // would skip the immediate runOnce as busy).
                if (accountId == currentAccountId_) {
                    serverExhaustedByLabel_.clear();
                    if (listModel_) {
                        listModel_->setFooterState(
                            fc::MessageListModel::FooterState::None);
                    }
                    if (sidebar_ && sidebar_->model()) {
                        sidebar_->model()->reload();
                    }
                    // Drive the message list through reloadCurrentLabel
                    // rather than replaceAll({}) — replaceAll resets
                    // the model's `source_` to None, after which the
                    // messagesUpdated handler's refreshFromSource bails
                    // and the view stays empty even after initial sync
                    // re-fills the cache.
                    if (currentLabelId_.isEmpty())
                        currentLabelId_ = QStringLiteral("INBOX");
                    reloadCurrentLabel();
                    refreshAccountIndicator();
                    refreshListFooter();
                }
                if (auto* ctx = accounts_->contextFor(accountId)) {
                    if (auto* s = ctx->sync()) {
                        postToObject(s, [s] { s->runOnce(); });
                    }
                    QPointer<MainWindow> self(this);
                    QTimer::singleShot(1500, this, [self, accountId] {
                        if (!self || !self->accounts_) return;
                        if (auto* c = self->accounts_->contextFor(accountId))
                            if (auto* s = c->sync()) {
                                postToObject(s, [s] { s->runOnce(); });
                            }
                    });
                }
            });
    connect(accounts_, &fc::account::AccountManager::accountSignedOut, this,
            [this](const QString& accountId) {
                // Re-run the gate (clears the UI if the last
                // authorized context just went away), refresh the
                // toolbar indicator + menu, and if the signed-out
                // account was the active one, promote another or
                // clear the panes.
                refreshAccountIndicator();
                refreshAccountMenu();
                if (accountId == currentAccountId_) {
                    QString next;
                    for (const auto& a : accounts_->accounts()) {
                        if (a.id == accountId) continue;
                        if (auto* c = accounts_->contextFor(a.id)) {
                            if (c->auth() && c->auth()->isAuthorized()) {
                                next = a.id; break;
                            }
                        }
                    }
                    if (!next.isEmpty()) {
                        accounts_->setCurrentAccountId(next);
                    } else {
                        clearAccountUiState();
                    }
                }
                enforceActiveAccountGate();
            });
    connect(accounts_, &fc::account::AccountManager::tokensLoaded, this,
            [this](const QString& accountId) {
                enforceActiveAccountGate();
                auto* ctx = accounts_->contextFor(accountId);
                if (!ctx || !ctx->auth() || !ctx->auth()->isAuthorized()) {
                    refreshAccountIndicator();
                    return;
                }
                if (auto* s = ctx->sync()) {
                    postToObject(s, [s] { s->runOnce(); s->startScheduler(); });
                }
                if (outbox_)  outbox_->start();
                if (pending_) pending_->start();
                if (drafts_)  drafts_->start();
                // App startup: enforceActiveAccountGate() ran before
                // keychain hydration completed and cleared the active
                // account because no context was authorized yet. Now
                // that THIS context has tokens, promote it as the
                // active account if there isn't one — that drives the
                // currentAccountChanged path which retargets the
                // sidebar's model, the message list's accountId, and
                // re-paints from cache. Add-account already calls
                // setCurrentAccountId in finalize, so this branch
                // only kicks in for the resume-existing-account
                // restart case.
                if (currentAccountId_.isEmpty()) {
                    // Resume case. Multiple per-context tokensLoaded
                    // signals can arrive in any order — the first to
                    // fire isn't necessarily the account
                    // AccountManager picked as current. Only adopt
                    // when this id matches AccountManager's choice
                    // (or when AccountManager hasn't picked one at
                    // all, which on startup means there's only this
                    // one account). Otherwise the wrong account
                    // claims the UI and the user has to toggle
                    // accounts to make the sidebar load.
                    const QString chosen = accounts_->currentAccountId();
                    if (!chosen.isEmpty() && chosen != accountId) {
                        refreshAccountIndicator();
                        return;
                    }
                    currentAccountId_ = accountId;
                    if (sync_) sync_->setAccountId(accountId);
                    if (sidebar_ && sidebar_->model())
                        sidebar_->model()->setAccountId(accountId);
                    if (listModel_) listModel_->setAccountId(accountId);
                    LabelStyleCache::instance().invalidate(accountId);
                    if (currentLabelId_.isEmpty()) {
                        currentLabelId_ = QStringLiteral("INBOX");
                    }
                    accounts_->setCurrentAccountId(accountId);
                    reloadCurrentLabel();
                    reloadSidebar();
                } else if (accountId == currentAccountId_) {
                    if (currentLabelId_.isEmpty()) {
                        currentLabelId_ = QStringLiteral("INBOX");
                    }
                    reloadCurrentLabel();
                    reloadSidebar();
                }
                refreshAccountIndicator();
                // Async-hydration path: the synchronous-auth branch in
                // the constructor saw no authorized context yet and
                // skipped restoreStartupSelection. Now that a context
                // finished hydrating, retry — the function self-gates
                // via startupSelectionApplied_ so it only takes effect
                // on the first eligible invocation.
                restoreStartupSelection();
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
                if (listModel_) listModel_->setAccountId(aid);
                LabelStyleCache::instance().invalidate(aid);
                listModel_->replaceAll({});
                reader_->showEmpty();
                currentMessage_ = {};
                currentRow_ = -1;
                if (!startupSelectionInProgress_) {
                    currentLabelId_ = QStringLiteral("INBOX");
                }
                reloadCurrentLabel();
                refreshAccountIndicator();
                // Skip persistence until restoreStartupSelection has
                // finished reading the saved tuple. Normal user-driven
                // switches persist after that.
                if (startupSelectionApplied_) persistLastViewedLabel();
            });
    connect(accounts_, &fc::account::AccountManager::labelsUpdated, this,
            [this](const QString& aid) {
                if (aid != currentAccountId_) return;
                LabelStyleCache::instance().invalidate(currentAccountId_);
                reloadSidebar();
            });
    // Background sync landing new rows + our own topUpLabel finishing
    // both come through here. The instinct is to call
    // reloadCurrentLabel — but that calls setLabelSource →
    // loadFirstPage which resets the model to offset=0 and snaps the
    // scroll bar to the top. With a 700-row IMPORTANT folder loaded,
    // every per-message upsert during a top-up was triggering a full
    // reset, which is why the scroll position jumped during
    // progressive loading. refreshFromSource re-queries the same
    // window the model already shows (limit = currently-loaded count)
    // so freshly-cached rows surface without losing the user's
    // position.
    connect(accounts_, &fc::account::AccountManager::messagesUpdated, this,
            [this](const QString& aid) {
                // Auto-prune: any account that just synced should be
                // re-checked against its bounds. Reads Preferences
                // here (UI thread, where they're available) and posts
                // the prune through the account SyncService so it runs
                // after the current update notification returns.
                // applyAutoPruneFor short-circuits internally when
                // all three caps are zero, so we skip the pre-gating
                // OR-check the previous shape had.
                if (Preferences::cacheAutoPrune() && accounts_
                    && !aid.isEmpty()) {
                    const int days = Preferences::cacheMaxAgeDays(aid);
                    const int msgs = Preferences::cacheMaxMessages(aid);
                    const int mb   = Preferences::cacheMaxCacheMb(aid);
                    if (auto* ctx = accounts_->contextFor(aid);
                            ctx && ctx->sync()) {
                        auto* accounts = accounts_;
                        postToObject(ctx->sync(),
                            [accounts, aid, days, msgs, mb] {
                                accounts->applyAutoPruneFor(
                                    aid, days, msgs, mb);
                            });
                    }
                }
                if (aid != currentAccountId_) return;
                if (!list_ || !listModel_) return;
                auto* sb = list_->verticalScrollBar();
                const int   y   = sb ? sb->value() : 0;
                const QString sel = currentMessage_.id;
                listModel_->refreshFromSource();
                if (sb) sb->setValue(y);
                if (!sel.isEmpty()) {
                    for (int i = 0; i < listModel_->rowCount(); ++i) {
                        const auto idx = listModel_->index(i, 0);
                        if (idx.data(fc::MessageListModel::IdRole)
                                .toString() == sel) {
                            list_->setCurrentIndex(idx);
                            break;
                        }
                    }
                }
                refreshListFooter();
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
                // Once Gmail has told us "no more for this label",
                // re-kicking topUpLabel is a free no-op for the
                // worker but a feedback loop here: the view stays
                // scrolled to the bottom, the model keeps emitting
                // cacheExhausted, and we'd keep posting topUpLabel
                // forever (the log fills with "kicking topUpLabel"
                // / "busy, skip" / "newRows=0 serverExhausted=1"
                // cycles). Mirror the same guard the scroll-bar
                // handler below uses.
                if (serverExhaustedByLabel_.value(labelId, false)) {
                    refreshListFooter();
                    return;
                }
                qInfo("MainWindow: cacheExhausted label='%s', kicking topUpLabel",
                      qUtf8Printable(labelId));
                if (auto* ctx = accounts_ ? accounts_->currentContext()
                                          : nullptr) {
                    if (auto* s = ctx->sync()) {
                        postToObject(s, [s, labelId] { s->topUpLabel(labelId); });
                    }
                } else if (sync_) {
                    sync_->topUpLabel(labelId);
                }
                refreshListFooter();
            });

    // User-initiated "scroll past loaded" trigger. resumeAfterTopUp can
    // leave the model in cacheDrained_=true (last drain chunk was a
    // short page) — at that point Qt's view won't auto-call fetchMore
    // because canFetchMore() returns false, and cacheExhausted will
    // not fire from a scroll. This handler does the same thing the
    // cacheExhausted path does (post a topUpLabel) but only when the
    // user's own scroll has reached the bottom of what's loaded AND
    // no top-up is already in flight AND the server hasn't reported
    // exhaustion for the current label. Without this, after a
    // resumeAfterTopUp drain ends with a short page the user would be
    // stuck — scrolling to the bottom does nothing because the cache
    // is dry and the model's hands are tied.
    if (auto* sb = list_->verticalScrollBar()) {
        connect(sb, &QScrollBar::valueChanged, this, [this](int value) {
            if (!list_ || !listModel_) return;
            auto* bar = list_->verticalScrollBar();
            if (!bar || bar->maximum() <= 0) return;
            // 32 px tolerance so the trigger fires when the user is
            // *near* the bottom, not strictly at the last pixel.
            if (bar->maximum() - value > 32) return;
            if (!listModel_->cacheDrained()) return;
            if (topUpsInFlight_ > 0) return;
            const QString labelId = listModel_->sourceLabelId();
            if (labelId.isEmpty()) return;
            if (serverExhaustedByLabel_.value(labelId, false)) return;
            if (auto* ctx = accounts_ ? accounts_->currentContext()
                                       : nullptr) {
                if (auto* s = ctx->sync()) {
                    postToObject(s, [s, labelId] {
                        s->topUpLabel(labelId);
                    });
                }
            }
        });
    }

    // Label-scoped progress messages for top-up. Generic stateChanged
    // already shows "Syncing…" / "Syncing… Done" for INITIAL +
    // INCREMENTAL passes; these layer on top with a label name when
    // the top-up is for the visible label, so the user can tell
    // "Syncing Receipts…" apart from a global background pass. Routed
    // through AccountManager so we listen to the *active* account's
    // per-context sync, not the dead anonymous Bootstrap sync_.
    connect(accounts_, &fc::account::AccountManager::topUpStarted, this,
            [this](const QString& aid, const QString& labelId) {
                qInfo("MainWindow: topUpStarted aid='%s' label='%s' "
                      "current='%s' sourceLabel='%s'",
                      qUtf8Printable(aid), qUtf8Printable(labelId),
                      qUtf8Printable(currentAccountId_),
                      qUtf8Printable(
                          listModel_ ? listModel_->sourceLabelId()
                                     : QString()));
                if (aid != currentAccountId_) return;
                const QString name = fc::cache::LabelRepository::byId(
                    currentAccountId_, labelId).name;
                statusBar()->showMessage(name.isEmpty()
                    ? tr("Syncing…")
                    : tr("Syncing %1…").arg(name));
                if (labelId == listModel_->sourceLabelId()) {
                    ++topUpsInFlight_;
                    refreshListFooter();
                }
            });
    connect(accounts_, &fc::account::AccountManager::topUpFinished, this,
            [this](const QString& aid, const QString& labelId,
                    int newRows, bool serverExhausted) {
                qInfo("MainWindow: topUpFinished aid='%s' label='%s' "
                      "newRows=%d serverExhausted=%d",
                      qUtf8Printable(aid), qUtf8Printable(labelId),
                      newRows, serverExhausted);
                if (aid != currentAccountId_) return;
                serverExhaustedByLabel_[labelId] = serverExhausted;
                // Decrement BEFORE resumeAfterTopUp: that call's
                // fetchMore may exhaust the cache again and post the
                // next topUpLabel synchronously inside this handler.
                // If we decremented after, the new topUpStarted's
                // increment would land, then ours would zero it out
                // and the footer would drop to None mid-chain.
                if (labelId == listModel_->sourceLabelId()
                    && topUpsInFlight_ > 0) {
                    --topUpsInFlight_;
                }
                // Only surface the "done" status message when the whole
                // chain has settled; while topUpsInFlight_ > 0 keep the
                // persistent "Syncing X..." message in place so the user
                // doesn't see a brief "up to date" flash before the next
                // chained top-up overwrites it.
                if (topUpsInFlight_ == 0) {
                    const QString name = fc::cache::LabelRepository::byId(
                        currentAccountId_, labelId).name;
                    if (!name.isEmpty()) {
                        const QString msg = newRows > 0
                            ? tr("%1: %n new", "", newRows).arg(name)
                            : tr("%1: up to date").arg(name);
                        statusBar()->showMessage(msg, 30000);
                    } else {
                        statusBar()->clearMessage();
                    }
                }
                // The cache just gained `newRows` older rows. Push them
                // into the model so the user's scroll-to-bottom session
                // continues seamlessly. Skip if the user has navigated
                // away to a different label since the top-up started.
                if (newRows > 0
                    && labelId == listModel_->sourceLabelId()) {
                    listModel_->resumeAfterTopUp();
                }
                refreshListFooter();
            });
    connect(accounts_, &fc::account::AccountManager::compressionPromptDue,
            this, &MainWindow::onCompressionPromptDue);
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

    // Per-account sync indicator. The `sync_->stateChanged` connection
    // above is on the anonymous Bootstrap stack which no longer ticks
    // post-multi-account-refactor; the real per-context sync emits
    // through AccountManager::syncStarted/syncFinished, which forward
    // every per-account SyncService::stateChanged(Idle ↔ non-Idle)
    // transition. Drives isSyncing_ and the empty-list footer
    // placeholder so the user sees "Syncing INBOX…" + "Loading more
    // messages…" during an initial sync after a cache wipe (where the
    // legacy connection would leave the UI silent).
    //
    // Top-ups also tick this via setState transitions inside
    // topUpLabelStep, but those are tracked separately by
    // topUpsInFlight_ — skip the isSyncing_ bookkeeping while a top-up
    // is in flight so the chained state flicker doesn't churn the
    // status bar.
    connect(accounts_, &fc::account::AccountManager::syncStarted, this,
            [this](const QString& aid) {
                if (aid != currentAccountId_) return;
                if (topUpsInFlight_ > 0) return;
                isSyncing_ = true;
                if (errorBanner_) errorBanner_->hide();
                const QString name = currentLabelId_.isEmpty()
                    ? QString()
                    : fc::cache::LabelRepository::byId(
                          currentAccountId_, currentLabelId_).name;
                statusBar()->showMessage(name.isEmpty()
                    ? tr("Syncing…")
                    : tr("Syncing %1…").arg(name));
                refreshListFooter();
            });
    connect(accounts_, &fc::account::AccountManager::syncFinished, this,
            [this](const QString& aid) {
                if (aid != currentAccountId_) return;
                if (topUpsInFlight_ > 0) return;
                const bool wasSyncing = isSyncing_;
                isSyncing_ = false;
                refreshListFooter();
                if (!wasSyncing) return;
                // Defer so a synchronous failed() handler firing after
                // this Idle transition can claim the slot.
                QTimer::singleShot(0, this, [this] {
                    if (lastSyncFailed_) {
                        lastSyncFailed_ = false;
                        return;
                    }
                    statusBar()->showMessage(tr("Syncing… Done"), 30000);
                });
            });

    // Restore the baseline ("Signed in as …" or "Syncing…" if a sync
    // is mid-flight) every time a temporary message expires. Without
    // this, a quick "Archived." toast clearing during sync would
    // leave the bar empty until the next state change.
    connect(statusBar(), &QStatusBar::messageChanged, this,
            [this](const QString& text) {
                if (!text.isEmpty()) return;
                if (isSyncing_ || topUpsInFlight_ > 0) {
                    const QString name = currentLabelId_.isEmpty()
                        ? QString()
                        : fc::cache::LabelRepository::byId(
                              currentAccountId_, currentLabelId_).name;
                    statusBar()->showMessage(name.isEmpty()
                        ? tr("Syncing…")
                        : tr("Syncing %1…").arg(name));
                    return;
                }
                // Read identity from the active context — auth_ alias
                // never hydrates after the Add-account refactor.
                auto* ctx = accounts_ ? accounts_->currentContext() : nullptr;
                QString email = (ctx && ctx->auth())
                    ? ctx->auth()->accountEmail()
                    : auth_->accountEmail();
                if (email.isEmpty() && !currentAccountId_.isEmpty()) {
                    email = accounts_->accountById(currentAccountId_).email;
                }
                statusBar()->showMessage(email.isEmpty()
                    ? tr("Not signed in.")
                    : tr("Signed in as %1").arg(email));
            });

    connect(outbox_, &fc::sync::OutboxWorker::itemSent, this,
            [this](qint64, const QString&) {
                statusBar()->showMessage(tr("Message sent."), 3000);
                if (auto* ctx = accounts_ ? accounts_->currentContext()
                                          : nullptr) {
                    if (auto* s = ctx->sync()) {
                        postToObject(s, [s] { s->runOnce(); });
                    }
                }
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

void MainWindow::refreshBandwidthLabel() {
    if (!bandwidthLabel_) return;
    const auto& s = fc::api::SessionTransfer::instance();
    const qint64 down = s.bytesIn();
    const qint64 up   = s.bytesOut();
    const int reqs    = s.requestCount();
    bandwidthLabel_->setText(QStringLiteral("↓ %1").arg(fc::util::humanBytes(down)));
    bandwidthLabel_->setAccessibleName(tr("Bandwidth used this session"));
    bandwidthLabel_->setToolTip(tr(
        "Session transfer since launch:\n"
        "↓ %1 received\n"
        "↑ %2 sent\n"
        "%3 request(s)")
        .arg(fc::util::humanBytes(down), fc::util::humanBytes(up))
        .arg(reqs));
}

void MainWindow::refreshAccountIndicator() {
    // Prefer the active account's context: auth_ is the Bootstrap
    // anonymous stack and stays empty even after a successful Add-
    // account. Fall back to auth_ for the brief pre-context window.
    auto* ctx = accounts_ ? accounts_->currentContext() : nullptr;
    QString email = (ctx && ctx->auth()) ? ctx->auth()->accountEmail()
                                         : auth_->accountEmail();
    if (email.isEmpty() && !currentAccountId_.isEmpty()) {
        email = fc::cache::MetaRepository::get(currentAccountId_,
                                               QStringLiteral("email"));
    }
    if (email.isEmpty()) {
        const auto info = accounts_->accountById(currentAccountId_);
        email = info.email;
    }
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

    // Read auth state from the active account's context — the
    // Bootstrap-time auth_ alias never hydrates after Add-account
    // because we drove the sign-in through a transient stack.
    auto* activeCtx = accounts_ ? accounts_->currentContext() : nullptr;
    auto* activeAuth = (activeCtx && activeCtx->auth())
        ? activeCtx->auth() : auth_;
    QString email = activeAuth ? activeAuth->accountEmail() : QString();
    if (email.isEmpty()) {
        email = fc::cache::MetaRepository::get(currentAccountId_,
                                               QStringLiteral("email"));
    }
    if (email.isEmpty() && !currentAccountId_.isEmpty()) {
        email = accounts_->accountById(currentAccountId_).email;
    }
    // "Signed in" if any per-account context has tokens, OR the
    // legacy anonymous auth_ is somehow authorized (shouldn't happen
    // post-cleanup but kept as a defensive check).
    bool signedIn = activeAuth && activeAuth->isAuthorized();
    if (!signedIn && accounts_) {
        for (auto* c : accounts_->allContexts()) {
            if (c && c->auth() && c->auth()->isAuthorized()) {
                signedIn = true; break;
            }
        }
    }

    accountButton_->setText(tr("Accounts"));
    accountButton_->setToolTip(signedIn && !email.isEmpty()
        ? tr("Signed in as %1").arg(email)
        : tr("Manage Google accounts"));

    // One menu entry per *signed-in* account — i.e., accounts whose
    // per-account OAuthClient currently has valid tokens. The accounts
    // table can also hold rows for accounts that were signed in once
    // and signed out (sign-out keeps the row by default). Listing all
    // of them in the toolbar would surface inactive accounts as usable
    // targets; filter to authorized ones here.
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
        beginAddAccountFlow();
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
                    if (!config_->isConfigured()) {
                        SetupWizard wiz(config_, this);
                        if (wiz.exec() != QDialog::Accepted) return;
                    }
                    beginAddAccountFlow();
                });
        dlg.exec();
    });

    // Cache manager used to have a second entry point in this menu;
    // removed to consolidate cache management under
    // Settings → Storage → Manage cache… so the user has exactly
    // one path to find it (and one place to maintain).
}

void MainWindow::onSignIn() {
    if (!config_->isConfigured()) {
        SetupWizard wiz(config_, this);
        if (wiz.exec() != QDialog::Accepted) return;
    }
    // First sign-in is the same flow as Add-account: a transient
    // unbound stack runs the OAuth dance, mints the accounts row from
    // the email returned by getProfile, and copies the tokens onto
    // the new AccountContext.
    beginAddAccountFlow();
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
// carry rows for sign-out-with-keep-cache; those rows do not imply an
// active account.
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
    if (listModel_) {
        listModel_->setAccountId({});
        listModel_->replaceAll({});
    }
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

    // Pop the cache-disposition prompt. Custom button labels make the
    // two sign-out variants explicit (delete-data vs keep-data) instead
    // of the older Yes/No mapping which the user had to read the body
    // copy to disambiguate.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Sign out"));
    const auto info = accounts_->accountById(accountId);
    box.setText(tr("Sign out of %1?")
                    .arg(info.email.isEmpty()
                         ? tr("this account") : info.email));
    box.setInformativeText(tr(
        "Deleting local account data will wipe cached messages, drafts, "
        "outbox, and labels for this account. The next sign-in will do "
        "a full initial sync."));
    auto* dropBtn = box.addButton(
        tr("Sign out and delete\nlocal account data"),
        QMessageBox::DestructiveRole);
    auto* keepBtn = box.addButton(tr("Sign out"),
                                   QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(keepBtn);
    // QMessageBox sizes to its informativeText; the custom destructive
    // button still needs its size hint, and the text column needs a
    // sensible width so the body does not make the button row sprawl.
    dropBtn->adjustSize();
    if (auto* layout = qobject_cast<QGridLayout*>(box.layout())) {
        layout->setColumnMinimumWidth(1, 420);
    }
    box.exec();
    auto* clicked = box.clickedButton();
    if (!clicked || clicked == box.button(QMessageBox::Cancel)) return;
    const bool dropCache = (clicked == dropBtn);
    Q_UNUSED(keepBtn);

    // Sign out the per-context OAuthClient — that's the slot the
    // tokens actually live in. The anonymous Bootstrap auth_ never
    // holds tokens, so signing it out would be a no-op that leaves
    // the active account's keychain entry untouched.
    if (auto* ctx = accounts_->contextFor(accountId)) {
        if (auto* a = ctx->auth()) {
            // Queue signOut through the account OAuthClient; it mutates
            // token state and may use QNetworkAccessManager to revoke
            // at Google.
            postToObject(a, [a] { a->signOut(); });
        }
    }
    if (accountId == currentAccountId_) {
        // Reset transient session state so a stale "current message"
        // / "current row" can't drive a shortcut press (r, e, #, …)
        // into operating on cached data that no longer represents
        // the signed-in user. Reader pane goes back to its empty
        // hint. Per-account cache + scheduler cleanup happens below.
        currentMessage_ = {};
        currentRow_     = -1;
        if (reader_) reader_->showEmpty(tr("Not signed in."));
    }

    // Stop the named context's scheduler. The shared workers stay
    // running; their drain queries simply skip rows for the now-
    // signed-out account once its context is gone.
    if (auto* ctx = accounts_->contextFor(accountId)) {
        if (auto* s = ctx->sync()) {
            postToObject(s, [s] { s->stopScheduler(); });
        }
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
    // Drive refresh through the active account's per-context sync,
    // not the Bootstrap-anonymous sync_ alias. After Add-account the
    // legacy auth_ never authorizes, so checking auth_->isAuthorized
    // here would always fall into onSignIn() and re-kick the OAuth
    // flow even when a real account is signed in.
    auto* ctx = accounts_ ? accounts_->currentContext() : nullptr;
    if (!ctx || !ctx->auth() || !ctx->auth()->isAuthorized()) {
        onSignIn();
        return;
    }
    statusBar()->showMessage(tr("Syncing…"));
    if (auto* s = ctx->sync()) {
        postToObject(s, [s] { s->runOnce(); });
    }
}

void MainWindow::onLabelSelected(const QString& accountId, const QString& id) {
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
    // Switch the active account when the user clicked a label that
    // lives under a different account branch. accountId is empty for
    // the cross-account synthetic; leave the current account alone in
    // that case (the cross-account view doesn't care which is active).
    if (!accountId.isEmpty() && accountId != currentAccountId_) {
        if (accounts_) accounts_->setCurrentAccountId(accountId);
        // setCurrentAccountId fires currentAccountChanged, which has
        // its own handler that does the model retargeting + sidebar
        // ellipsis hint update. We fall through here to actually
        // navigate to the chosen label.
    }
    // Bail if neither the cross-account flip nor the underlying
    // label changed — repeated clicks on the same row should be a
    // no-op rather than a full reload.
    if (!wasCross && !crossAccountView_ && resolvedId == currentLabelId_
        && accountId == currentAccountId_) return;
    // Save the outgoing label's scroll position before swapping the
    // model out from under us; we'll try to restore it the next time
    // the user comes back to that label.
    saveLabelScrollState();
    currentLabelId_ = resolvedId;
    // Reset top-up state for the new label — any in-flight top-up
    // for the old label is no longer interesting to the footer.
    topUpsInFlight_ = 0;
    refreshListFooter();
    reloadCurrentLabel();
    // After reloadCurrentLabel has populated the model for the new
    // label, restore scroll position (or land at the top if we have
    // no memory for this label).
    restoreLabelScrollState(resolvedId, crossAccountView_);
    // Initial sync only seeds INBOX / SENT / DRAFT / STARRED — every
    // other label (every user label, plus categories like SPAM /
    // TRASH) only carries cached rows for messages that happened to
    // overlap with a seed at sync time. Pull a server-side page so
    // the user sees what Gmail web sees, not just the lucky overlap.
    // SyncService::topUpLabel itself is a no-op for the seed labels
    // and for the empty / search-mode case.
    if (currentSearchQuery_.isEmpty()) {
        if (auto* ctx = accounts_ ? accounts_->currentContext() : nullptr) {
            if (auto* s = ctx->sync()) {
                postToObject(s, [s, id] { s->topUpLabel(id); });
            }
        }
    }
    refreshListFooter();
    persistLastViewedLabel();
}

void MainWindow::reloadSidebar() {
    sidebar_->model()->reload();
}

void MainWindow::persistLastViewedLabel() {
    if (currentLabelId_.isEmpty()) return;
    Preferences::setLastViewedSelection({
        crossAccountView_ ? QString() : currentAccountId_,
        currentLabelId_,
        crossAccountView_});
}

void MainWindow::restoreStartupSelection() {
    if (startupSelectionApplied_) return;
    if (!accounts_) return;

    auto isAuthorized = [this](const QString& id) -> bool {
        auto* c = id.isEmpty() ? nullptr : accounts_->contextFor(id);
        return c && c->auth() && c->auth()->isAuthorized();
    };
    bool anyAuth = false;
    for (auto* c : accounts_->allContexts()) {
        if (c && c->auth() && c->auth()->isAuthorized()) { anyAuth = true; break; }
    }
    if (!anyAuth) return;

    // Saved tuple is "valid" if the saved account still exists in
    // the accounts list — authorization may lag (per-account keychain
    // hydration is async) and the label cache loads regardless.
    auto saved = Preferences::lastViewedSelection();
    bool valid = saved.crossAccountView ? !saved.labelId.isEmpty()
                                         : !saved.accountId.isEmpty()
                                             && !saved.labelId.isEmpty()
                                             && accounts_->accountById(saved.accountId).id
                                                  == saved.accountId;
    if (!valid) {
        saved.crossAccountView = false;
        saved.labelId          = QStringLiteral("INBOX");
        saved.accountId.clear();
        for (const auto& a : accounts_->accounts()) {
            if (isAuthorized(a.id)) { saved.accountId = a.id; break; }
        }
        if (saved.accountId.isEmpty()) return;
    }

    startupSelectionApplied_ = true;

    if (saved.crossAccountView) {
        crossAccountView_ = true;
        currentLabelId_   = saved.labelId;
        reloadCurrentLabel();
        reloadSidebar();
        if (sidebar_) {
            sidebar_->selectLabel(QString(), QStringLiteral("__all_inboxes"));
        }
    } else {
        crossAccountView_ = false;
        currentLabelId_   = saved.labelId;
        // Switch first so the currentAccountChanged handler does the
        // sole reloadCurrentLabel — gated by startupSelectionInProgress_
        // so the handler doesn't clobber currentLabelId_ back to INBOX.
        if (saved.accountId != currentAccountId_) {
            QScopedValueRollback<bool> guard(startupSelectionInProgress_, true);
            accounts_->setCurrentAccountId(saved.accountId);
        } else {
            reloadCurrentLabel();
        }
        reloadSidebar();
        if (sidebar_) sidebar_->selectLabel(saved.accountId, saved.labelId);
    }
    persistLastViewedLabel();
}

QString MainWindow::labelScrollKey(const QString& labelId,
                                    bool crossAccount) const {
    // Cross-account "All Inboxes" gets its own key so it doesn't
    // share state with any single account's INBOX. Otherwise we
    // namespace by the active account so each account remembers its
    // own per-folder scroll position independently.
    if (crossAccount) return QStringLiteral("__all::") + labelId;
    return currentAccountId_ + QStringLiteral("::") + labelId;
}

void MainWindow::saveLabelScrollState() {
    if (!list_ || !listModel_) return;
    if (currentLabelId_.isEmpty()) return;
    const QString key = labelScrollKey(currentLabelId_, crossAccountView_);
    LabelScrollState s;
    s.verticalOffset = list_->verticalScrollBar()
        ? list_->verticalScrollBar()->value() : 0;
    s.loadedRowCount = listModel_->rowCount();
    if (currentRow_ >= 0 && currentRow_ < listModel_->rowCount()) {
        const auto idx = listModel_->index(currentRow_, 0);
        s.messageId = idx.data(fc::MessageListModel::IdRole).toString();
    }
    labelScrollMemory_.insert(key, s);
}

void MainWindow::restoreLabelScrollState(const QString& labelId,
                                          bool crossAccount) {
    if (!list_ || !listModel_) return;
    const QString key = labelScrollKey(labelId, crossAccount);
    const auto it = labelScrollMemory_.constFind(key);
    if (it == labelScrollMemory_.constEnd()) {
        // No memory for this label — start at the top so the user
        // sees the most recent message instead of inheriting the
        // previous label's scroll offset.
        if (auto* sb = list_->verticalScrollBar()) sb->setValue(0);
        return;
    }
    const auto& s = it.value();
    // Expand the loaded window first — setLabelSource just loaded
    // pageSize rows, but the user previously had thousands. Without
    // this, the saved messageId/offset usually points past the end
    // of what's currently in the model and progressive loading has
    // to re-walk the whole label.
    if (s.loadedRowCount > listModel_->rowCount()) {
        listModel_->expandLoadedRows(s.loadedRowCount);
    }
    // Prefer message-id restoration (model rows can shift between
    // visits if sync brought new messages in). Fall back to the raw
    // scroll offset if we can't find the saved id.
    if (!s.messageId.isEmpty()) {
        for (int i = 0; i < listModel_->rowCount(); ++i) {
            const auto idx = listModel_->index(i, 0);
            if (idx.data(fc::MessageListModel::IdRole).toString()
                    == s.messageId) {
                list_->scrollTo(idx, QAbstractItemView::PositionAtCenter);
                return;
            }
        }
    }
    if (auto* sb = list_->verticalScrollBar()) sb->setValue(s.verticalOffset);
}

fc::api::GmailClient* MainWindow::activeGmail() const {
    if (accounts_) {
        if (auto* ctx = accounts_->currentContext()) {
            if (ctx->gmail()) return ctx->gmail();
        }
    }
    return gmail_;
}

void MainWindow::refreshListFooter() {
    if (!listModel_) return;

    using FooterState = fc::MessageListModel::FooterState;
    FooterState target = FooterState::None;

    if (topUpsInFlight_ > 0) {
        target = FooterState::Loading;
    } else if (isSyncing_) {
        // Active sync on the current account — surface the
        // "Loading more messages..." placeholder regardless of how
        // many rows are already loaded. The toolbar spinner alone
        // wasn't enough feedback; the user explicitly wants the
        // in-list message any time the list they're looking at is
        // being synced.
        target = FooterState::Loading;
    } else {
        const QString labelId = listModel_->sourceLabelId();
        if (!labelId.isEmpty() && listModel_->cacheDrained()) {
            const bool srvDone =
                serverExhaustedByLabel_.value(labelId, false);
            if (srvDone) target = FooterState::NoMore;
        }
    }
    listModel_->setFooterState(target);
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
    } else if (currentLabelId_ == QStringLiteral("__all_mail")) {
        // Synthetic "All Mail" view — every cached message except
        // SPAM/TRASH for the active account. Server-side top-up
        // routes through SyncService::topUpLabel("__all_mail"),
        // which maps to a Gmail messages.list call with no label
        // filter (the API excludes SPAM/TRASH by default).
        listModel_->setAllMailSource(conv, Preferences::unreadOnly());
    } else if (currentSearchQuery_.isEmpty()) {
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
        && (!cached.hasAttachment   || !cached.attachments.empty())
        // body_compression=2 = orphan: bytes are zstd_dict_v1 but
        // compressed against a dict we no longer have. Treat as
        // missing and re-fetch from Gmail — the upsert that lands
        // the fresh plaintext will clear the orphan flag.
        && cached.bodyCompression != 2;

    if (cacheLooksComplete) {
        renderThread(cached);
        return;
    }

    reader_->showLoading();
    QPointer<MainWindow> self(this);
    // For cross-account browsing the message belongs to whichever
    // account owns the row, not the toolbar selection — same logic
    // as `lookupAccount` above. Stamping with currentAccountId_
    // could land an empty string ("All Inboxes" before any account
    // is selected) and trigger the dropped-without-accountId path.
    const QString accountForFetch = lookupAccount;
    fc::api::GmailClient* fetchGmail = activeGmail();
    if (accounts_ && !lookupAccount.isEmpty()) {
        if (auto* fc = accounts_->contextFor(lookupAccount)) {
            if (fc->gmail()) fetchGmail = fc->gmail();
        }
    }
    // fetchGmail lives with its AccountContext on the UI thread. Queue the
    // call so the network request starts after the current selection/render
    // pass, then touch widgets only from the final UI callback.
    postToObject(fetchGmail, [fetchGmail, messageId, self, renderThread,
                              accountForFetch] {
        fetchGmail->getMessage(messageId,
            [self, renderThread, accountForFetch]
            (fc::Message m, fc::api::ApiError err) {
                // Callback runs on the GmailClient's thread, currently
                // the UI thread. Keep the final invokeMethod so the
                // destroyed-mid-flight case is guarded by the QPointer
                // null check inside the lambda.
                if (!err && !m.id.isEmpty()) {
                    m.accountId = accountForFetch;
                    fc::cache::MessageRepository::upsert(accountForFetch, m);
                }
                if (!self) return;
                QMetaObject::invokeMethod(self.data(),
                    [self, renderThread, m, err] {
                        if (!self) return;
                        if (err) {
                            self->reader_->showEmpty(
                                tr("Failed to load: %1").arg(err.message));
                            return;
                        }
                        renderThread(m);
                    }, Qt::QueuedConnection);
            });
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
    fc::api::GmailClient* gmail = activeGmail();
    // gmail lives with its AccountContext on the UI thread. Queue the
    // listMessages call so the request starts after search-submit UI
    // handling returns. The hydration loop inside the callback fires
    // more getMessage calls on that same object; UI updates still go
    // through invokeMethod(self) for lifetime guarding.
    postToObject(gmail, [gmail, q, self, accountForSearch] {
        gmail->listMessages({}, q, {}, kPageSize,
            [self, gmail, q, accountForSearch]
            (fc::api::GmailClient::ListPage page, fc::api::ApiError err) {
                if (err) {
                    if (!self) return;
                    QMetaObject::invokeMethod(self.data(),
                        [self, err] {
                            if (!self) return;
                            self->statusBar()->showMessage(
                                tr("Server search failed: %1").arg(err.message));
                        }, Qt::QueuedConnection);
                    return;
                }
                // Hydrate any missing ids from the same GmailClient
                // callback path before refreshing the UI.
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
                const int idCount = static_cast<int>(page.ids.size());
                if (!self) return;
                QMetaObject::invokeMethod(self.data(),
                    [self, q, idCount] {
                        if (!self) return;
                        self->statusBar()->showMessage(
                            tr("Server returned %1 ids; cache updates in background.")
                                .arg(idCount));
                        self->currentSearchQuery_ = q;
                        self->reloadCurrentLabel();
                    }, Qt::QueuedConnection);
            });
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
    fc::Message m = fc::cache::MessageRepository::byId(currentAccountId_, messageId);
    if (m.id.isEmpty()) return;
    openComposeWindow(&m, int(ComposeWindow::Mode::Reply));
}

void MainWindow::onReplyAllToMessage(const QString& messageId) {
    if (messageId.isEmpty()) return;
    fc::Message m = fc::cache::MessageRepository::byId(currentAccountId_, messageId);
    if (m.id.isEmpty()) return;
    openComposeWindow(&m, int(ComposeWindow::Mode::ReplyAll));
}

void MainWindow::onForwardMessage(const QString& messageId) {
    if (messageId.isEmpty()) return;
    fc::Message m = fc::cache::MessageRepository::byId(currentAccountId_, messageId);
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
    fc::cache::MessageRepository::applyLabelDiff(currentAccountId_, messageId, {}, rem);
    fc::cache::PendingOpsRepository::enqueueModify(currentAccountId_, messageId, {}, rem);
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
    fc::cache::MessageRepository::applyLabelDiff(currentAccountId_, messageId, add, rem);
    fc::cache::PendingOpsRepository::enqueueModify(currentAccountId_, messageId, add, rem);
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
    fc::cache::MessageRepository::applyLabelDiff(currentAccountId_, messageId, add, rem);
    fc::cache::PendingOpsRepository::enqueueModify(currentAccountId_, messageId, add, rem);
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
    fc::cache::MessageRepository::setSnoozeUntil(currentAccountId_, messageId, wakeAt);
    const QStringList rem{QStringLiteral("INBOX")};
    fc::cache::MessageRepository::applyLabelDiff(currentAccountId_, messageId, {}, rem);
    fc::cache::PendingOpsRepository::enqueueModify(currentAccountId_, messageId, {}, rem);
    pending_->flush();
    statusBar()->showMessage(
        tr("Snoozed until %1.").arg(when.toString(QStringLiteral("MMM d, h:mm AP"))),
        4000);
    reloadCurrentLabel();
}

void MainWindow::onCreateLabel(const QString& accountIdArg,
                                const QString& parentLabelId) {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New label"),
        tr("Label name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.isEmpty()) return;

    // Per-account create: route to whichever account branch the user
    // right-clicked in. Falls back to currentAccountId_ for legacy
    // call sites (e.g. cross-account "__all_inboxes" with no aid).
    const QString targetAccount = accountIdArg.isEmpty()
        ? currentAccountId_ : accountIdArg;
    const auto parent = fc::cache::LabelRepository::byId(targetAccount,
                                                         parentLabelId);
    if (!parent.id.isEmpty() && parent.type == QLatin1String("user")) {
        name = parent.name + QLatin1Char('/') + name;
    }
    QPointer<MainWindow> self(this);
    const QString accountForCreate = targetAccount;
    fc::api::GmailClient* gmail = nullptr;
    if (auto* ctx = accounts_ ? accounts_->contextFor(targetAccount)
                              : nullptr) {
        gmail = ctx->gmail();
    }
    if (!gmail) gmail = activeGmail();   // legacy fallback
    // Queue createLabel through the GmailClient event loop and marshal
    // UI work back once the API call lands.
    postToObject(gmail, [gmail, name, self, accountForCreate] {
        gmail->createLabel(name,
            [self, accountForCreate]
            (fc::api::GmailClient::Label l, fc::api::ApiError err) {
                if (!err) {
                    fc::cache::LabelRow row;
                    row.accountId = accountForCreate;
                    row.id        = l.id;
                    row.name      = l.name;
                    row.type      = l.type;
                    fc::cache::LabelRepository::upsert(accountForCreate, row);
                }
                if (!self) return;
                QMetaObject::invokeMethod(self.data(), [self, err] {
                    if (!self) return;
                    if (err) {
                        QMessageBox::warning(self, tr("Create label"),
                                             err.message);
                        return;
                    }
                    self->reloadSidebar();
                }, Qt::QueuedConnection);
            });
    });
}

void MainWindow::onRenameLabel(const QString& accountIdArg,
                                const QString& labelId) {
    if (fc::util::DryRun::block(QStringLiteral("rename-label"))) {
        statusBar()->showMessage(
            tr("Dry-run mode: label rename blocked."), 4000);
        return;
    }
    const QString targetAccount = accountIdArg.isEmpty()
        ? currentAccountId_ : accountIdArg;
    const auto current = fc::cache::LabelRepository::byId(targetAccount,
                                                          labelId);
    if (current.id.isEmpty() || current.type != QLatin1String("user")) return;

    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("Rename label"),
        tr("New name:"), QLineEdit::Normal, current.name, &ok);
    if (!ok || newName.isEmpty() || newName == current.name) return;

    QPointer<MainWindow> self(this);
    const QString accountForRename = targetAccount;
    fc::api::GmailClient* gmail = nullptr;
    if (auto* ctx = accounts_ ? accounts_->contextFor(targetAccount)
                              : nullptr) {
        gmail = ctx->gmail();
    }
    if (!gmail) gmail = activeGmail();
    postToObject(gmail, [gmail, labelId, newName, self, accountForRename] {
        gmail->updateLabel(labelId, newName,
            [self, labelId, newName, accountForRename]
            (fc::api::GmailClient::Label, fc::api::ApiError err) {
                if (!err) {
                    auto row = fc::cache::LabelRepository::byId(
                        accountForRename, labelId);
                    row.name = newName;
                    fc::cache::LabelRepository::upsert(accountForRename, row);
                }
                if (!self) return;
                QMetaObject::invokeMethod(self.data(), [self, err] {
                    if (!self) return;
                    if (err) {
                        QMessageBox::warning(self, tr("Rename label"),
                                             err.message);
                        return;
                    }
                    self->reloadSidebar();
                }, Qt::QueuedConnection);
            });
    });
}

void MainWindow::onDeleteLabel(const QString& accountIdArg,
                                const QString& labelId) {
    if (fc::util::DryRun::block(QStringLiteral("delete-label"))) {
        statusBar()->showMessage(
            tr("Dry-run mode: label deletion blocked."), 4000);
        return;
    }
    const QString targetAccount = accountIdArg.isEmpty()
        ? currentAccountId_ : accountIdArg;
    const auto current = fc::cache::LabelRepository::byId(targetAccount,
                                                          labelId);
    if (current.id.isEmpty() || current.type != QLatin1String("user")) return;
    if (QMessageBox::question(this, tr("Delete label"),
            tr("Delete the label '%1'? Messages keep their other labels.")
              .arg(current.name)) != QMessageBox::Yes) return;

    QPointer<MainWindow> self(this);
    const QString accountForDelete = targetAccount;
    fc::api::GmailClient* gmail = nullptr;
    if (auto* ctx = accounts_ ? accounts_->contextFor(targetAccount)
                              : nullptr) {
        gmail = ctx->gmail();
    }
    if (!gmail) gmail = activeGmail();
    postToObject(gmail, [gmail, labelId, self, accountForDelete] {
        gmail->deleteLabel(labelId,
            [self, labelId, accountForDelete](fc::api::ApiError err) {
                if (!err) {
                    fc::cache::LabelRepository::remove(accountForDelete, labelId);
                }
                if (!self) return;
                QMetaObject::invokeMethod(self.data(), [self, err] {
                    if (!self) return;
                    if (err) {
                        QMessageBox::warning(self, tr("Delete label"),
                                             err.message);
                        return;
                    }
                    self->reloadSidebar();
                }, Qt::QueuedConnection);
            });
    });
}

void MainWindow::onCacheLabel(const QString& accountIdArg,
                                const QString& labelId) {
    if (accountIdArg.isEmpty() || labelId.isEmpty()) return;
    if (!accounts_) return;
    auto* ctx = accounts_->contextFor(accountIdArg);
    if (!ctx || !ctx->sync()) {
        QMessageBox::warning(this, tr("Cache label"),
            tr("This account isn't ready to sync."));
        return;
    }

    const auto label = fc::cache::LabelRepository::byId(accountIdArg,
                                                          labelId);
    const QString display = label.name.isEmpty() ? labelId : label.name;
    const QString email = accounts_->accountById(accountIdArg).email;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Cache all messages"));
    box.setText(tr("Download and cache every message in '%1' for %2?")
                  .arg(display, email.isEmpty() ? accountIdArg : email));
    box.setInformativeText(tr(
        "FirstContact will walk this label end-to-end, pulling each "
        "page from Gmail. Large folders can take a while and will "
        "burn Gmail quota. You can keep using the app; cancel from "
        "Settings if needed."));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Yes);
    if (box.exec() != QMessageBox::Yes) return;

    auto* sync = ctx->sync();
    postToObject(sync, [sync, labelId] {
        sync->cacheLabelComplete(labelId);
    });
    statusBar()->showMessage(
        tr("Caching '%1'…").arg(display), 10000);
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
    fc::api::GmailClient* gmail = activeGmail();
    // Queue the API call through the GmailClient event loop. The callback
    // can do its temp-file write inline (POSIX file I/O), but the status-bar
    // + QDesktopServices::openUrl work stays on the UI callback path.
    postToObject(gmail, [gmail, messageId, attachmentId, filename, self] {
        gmail->getAttachment(messageId, attachmentId,
            [self, filename](QByteArray bytes, fc::api::ApiError err) {
                QString writeErr;
                QString target;
                bool writeOk = false;
                if (!err) {
                    target = uniqueTargetPath(sessionTempDir(), filename);
                    writeOk = writeBytesToPath(target, bytes, &writeErr);
                }
                if (!self) return;
                QMetaObject::invokeMethod(self.data(),
                    [self, err, writeErr, writeOk, target] {
                        if (!self) return;
                        if (err) {
                            self->statusBar()->showMessage(
                                tr("Open failed: %1").arg(err.message), 8000);
                            return;
                        }
                        if (!writeOk) {
                            self->statusBar()->showMessage(
                                tr("Couldn't write temp file: %1").arg(writeErr),
                                8000);
                            return;
                        }
                        // Intentionally NOT calling
                        // AttachmentRepository::markDownloaded — these temp
                        // files aren't a permanent download.
                        self->statusBar()->showMessage(
                            tr("Opened %1").arg(QFileInfo(target).fileName()),
                            5000);
                        QDesktopServices::openUrl(QUrl::fromLocalFile(target));
                    }, Qt::QueuedConnection);
            });
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
    fc::api::GmailClient* gmail = activeGmail();
    postToObject(gmail, [gmail, messageId, attachmentId, target, self,
                          accountForSave] {
        gmail->getAttachment(messageId, attachmentId,
            [self, attachmentId, target, accountForSave]
            (QByteArray bytes, fc::api::ApiError err) {
                QString writeErr;
                bool writeOk = false;
                if (!err) {
                    writeOk = writeBytesToPath(target, bytes, &writeErr);
                    if (writeOk) {
                        fc::cache::AttachmentRepository::markDownloaded(
                            accountForSave, attachmentId, target);
                    }
                }
                if (!self) return;
                QMetaObject::invokeMethod(self.data(),
                    [self, err, writeErr, writeOk, target] {
                        if (!self) return;
                        if (err) {
                            self->statusBar()->showMessage(
                                tr("Download failed: %1").arg(err.message), 8000);
                            return;
                        }
                        if (!writeOk) {
                            self->statusBar()->showMessage(
                                tr("Couldn't write %1: %2").arg(target, writeErr),
                                8000);
                            return;
                        }
                        // No auto-open here — the user explicitly chose
                        // Save as…, so we respect that and don't pop a
                        // viewer afterwards.
                        self->statusBar()->showMessage(
                            tr("Saved to %1").arg(target), 8000);
                    }, Qt::QueuedConnection);
            });
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
    fc::api::GmailClient* gmail = activeGmail();
    for (const auto& a : m.attachments) {
        if (a.id.isEmpty()) continue;   // inline-only; not addressable
        const QString target = uniqueTargetPath(dir, a.filename);
        const QString attachmentId = a.id;
        const QString filename     = a.filename;
        postToObject(gmail, [gmail, messageId, attachmentId, target, filename,
                              self, accountForBatch] {
            gmail->getAttachment(messageId, attachmentId,
                [self, attachmentId, target, filename, accountForBatch]
                (QByteArray bytes, fc::api::ApiError err) {
                    QString writeErr;
                    bool writeOk = false;
                    if (!err) {
                        writeOk = writeBytesToPath(target, bytes, &writeErr);
                        if (writeOk) {
                            fc::cache::AttachmentRepository::markDownloaded(
                                accountForBatch, attachmentId, target);
                        }
                    }
                    if (!self) return;
                    QMetaObject::invokeMethod(self.data(),
                        [self, err, writeErr, writeOk, target, filename] {
                            if (!self) return;
                            if (err) {
                                self->statusBar()->showMessage(
                                    tr("Download failed (%1): %2")
                                        .arg(filename, err.message), 8000);
                                return;
                            }
                            if (!writeOk) {
                                self->statusBar()->showMessage(
                                    tr("Couldn't write %1: %2")
                                        .arg(target, writeErr), 8000);
                                return;
                            }
                            self->statusBar()->showMessage(
                                tr("Saved %1").arg(QFileInfo(target).fileName()),
                                4000);
                        }, Qt::QueuedConnection);
                });
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
    sidebar_->selectLabel(currentAccountId_, labelId);
}

void MainWindow::onCompressionPromptDue(const QString& accountId,
                                          int bodyCount) {
    if (accountId.isEmpty()) return;
    // Final pre-dialog gating. SyncService also gates internally, but
    // the user could have disabled compression between SyncService's
    // signal emit and this slot running, or another account could
    // have already accepted compression and we don't want to re-ask.
    if (!Preferences::dbCompression()) return;
    if (Preferences::dbCompressionPromptShown(accountId)) return;
    if (compressionWorkers_.value(accountId)) return;   // already running
    if (!fc::cache::MessageRepository::dictionaryFor(accountId).isEmpty()) {
        Preferences::setDbCompressionPromptShown(accountId, true);
        return;
    }

    QString email;
    if (accounts_) email = accounts_->accountById(accountId).email;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Compress message database?"));
    box.setText(tr("FirstContact can now train a custom compression "
                    "dictionary against your cached messages for "
                    "%1.\n\nWith %n message(s) on disk you should see "
                    "a 3-5× reduction in body storage. The "
                    "process runs in the background but the message "
                    "database will be unusually busy while it works; "
                    "you can keep using the app.", "", bodyCount)
                  .arg(email.isEmpty() ? accountId : email));
    box.setInformativeText(tr(
        "You can change this later in Settings → Storage."));
    auto* compressBtn  = box.addButton(tr("Compress database"),
                                        QMessageBox::AcceptRole);
    auto* disableBtn   = box.addButton(tr("Disable DB compression"),
                                        QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);   // ask again later
    box.setDefaultButton(compressBtn);
    box.exec();
    auto* chosen = box.clickedButton();
    Preferences::setDbCompressionPromptShown(accountId, true);
    if (chosen == compressBtn) {
        startCompressionWorker(accountId,
            fc::cache::BodyCompressionWorker::Mode::InitialTrain);
    } else if (chosen == disableBtn) {
        Preferences::setDbCompression(false);
    }
    // Cancel = do nothing; the dialog won't pop again this session.
}

void MainWindow::onRecompressAllAccounts() {
    if (!accounts_) return;
    QStringList eligible;
    int totalRows = 0;
    QStringList lines;
    for (const auto& a : accounts_->accounts()) {
        auto* c = accounts_->contextFor(a.id);
        if (!c || !c->auth() || !c->auth()->isAuthorized()) continue;
        const int rows = fc::cache::MessageRepository::bodyCountFor(a.id);
        if (rows == 0) continue;
        eligible << a.id;
        totalRows += rows;
        lines << tr("  • %1 — %n message(s)", "", rows)
                  .arg(a.email.isEmpty() ? a.id : a.email);
    }
    if (eligible.isEmpty()) {
        QMessageBox::information(this, tr("Recompress"),
            tr("No signed-in accounts have cached bodies to recompress."));
        return;
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Recompress message database"));
    box.setText(tr("Recompress every signed-in account?"));
    box.setInformativeText(tr(
        "Per account:\n%1\n\n"
        "Total: %2 message(s). Accounts run sequentially; the "
        "progress dialog stays open per account so you can watch "
        "each pass and see any errors.")
        .arg(lines.join(QStringLiteral("\n")))
        .arg(QLocale::system().toString(totalRows)));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes) return;

    recompressPending_ = eligible;
    dequeueNextRecompress();
}

void MainWindow::dequeueNextRecompress() {
    if (recompressPending_.isEmpty()) return;
    const QString next = recompressPending_.takeFirst();
    // Skip accounts whose worker is already in flight (shouldn't
    // happen during a batch, but a stray manual recompress could).
    if (compressionWorkers_.value(next)) {
        dequeueNextRecompress();
        return;
    }
    startCompressionWorker(next,
        fc::cache::BodyCompressionWorker::Mode::Recompress);
}

void MainWindow::onRecompressRequested(const QString& accountId) {
    if (accountId.isEmpty()) return;
    if (compressionWorkers_.value(accountId)) {
        QMessageBox::information(this, tr("Recompress"),
            tr("A compression pass is already running for this "
               "account. Wait for it to finish before starting "
               "another."));
        return;
    }
    const int rows = fc::cache::MessageRepository::bodyCountFor(accountId);
    QString email;
    if (accounts_) email = accounts_->accountById(accountId).email;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Recompress message database"));
    box.setText(tr("Recompressing %1 will rebuild the compression "
                    "dictionary from a fresh sample and rewrite every "
                    "body in the cache (%n message(s) on disk).",
                    "", rows)
                  .arg(email.isEmpty() ? accountId : email));
    box.setInformativeText(tr(
        "This is a long, single-threaded operation tuned for low "
        "memory and disk usage; expect several minutes per gigabyte "
        "of cache. You can keep using the app while it runs."));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes) return;
    startCompressionWorker(accountId,
        fc::cache::BodyCompressionWorker::Mode::Recompress);
}

void MainWindow::startCompressionWorker(
        const QString& accountId,
        fc::cache::BodyCompressionWorker::Mode mode) {
    auto* w = new fc::cache::BodyCompressionWorker(accountId, mode);
    compressionWorkers_.insert(accountId, w);

    // The progress dialog is the user-visible surface: it sits on top
    // of whatever dialog kicked off the compress (Settings/Storage)
    // so errors aren't swallowed behind another modal window. Status
    // bar still gets a single "Started/Done" line for the running
    // record, but every step also lands in the dialog.
    auto* dlg = new fc::ui::CompressionProgressDialog(accounts_,
                                                       accountId, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->attachWorker(w);

    QPointer<MainWindow> self(this);
    connect(w, &fc::cache::BodyCompressionWorker::finished, this,
            [self, accountId](const QString&, int rewroteCount,
                               qint64 savedBytes) {
                if (!self) return;
                self->compressionWorkers_.remove(accountId);
                const QString human =
                    savedBytes >= (10 * 1024 * 1024)
                    ? tr("%1 MB").arg(savedBytes / (1024 * 1024))
                    : tr("%1 KB").arg(qMax<qint64>(1, savedBytes / 1024));
                self->statusBar()->showMessage(
                    tr("Compression done: %1 message(s) rewritten, "
                       "%2 reclaimed.").arg(rewroteCount).arg(human),
                    15000);
                // No-op when recompressPending_ is empty (single-
                // account path); kicks the next account otherwise.
                self->dequeueNextRecompress();
            });
    connect(w, &fc::cache::BodyCompressionWorker::failed, this,
            [self, accountId](const QString&, const QString& reason) {
                if (!self) return;
                self->compressionWorkers_.remove(accountId);
                self->statusBar()->showMessage(
                    tr("Compression failed: %1").arg(reason), 15000);
                self->dequeueNextRecompress();
            });

    w->start();
    statusBar()->showMessage(tr("Starting database compression…"), 5000);
    dlg->show();
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
    const QString lookupAccount = currentMessage_.accountId.isEmpty()
        ? currentAccountId_ : currentMessage_.accountId;
    const auto cached = fc::cache::MessageRepository::byId(lookupAccount,
                                                           currentMessage_.id);
    if (cached.id.isEmpty()) return;
    const auto thread = fc::cache::MessageRepository::byThread(lookupAccount,
                                                               cached.threadId);
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
    const QString threadAccount = currentMessage_.accountId.isEmpty()
        ? currentAccountId_ : currentMessage_.accountId;
    const auto messages = fc::cache::MessageRepository::byThread(
                              threadAccount, currentMessage_.threadId);
    QSet<QString> applied;
    for (const auto& m : messages) {
        for (const auto& id : m.labelIds) applied.insert(id);
    }

    LabelChooserDialog dlg(LabelChooserDialog::Mode::Apply, currentAccountId_,
                            applied, this);
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

    const QString threadAccount = currentMessage_.accountId.isEmpty()
        ? currentAccountId_ : currentMessage_.accountId;
    const auto messages = fc::cache::MessageRepository::byThread(
                              threadAccount, currentMessage_.threadId);
    QSet<QString> applied;
    for (const auto& m : messages) {
        for (const auto& id : m.labelIds) applied.insert(id);
    }

    LabelChooserDialog dlg(LabelChooserDialog::Mode::MoveTo, currentAccountId_,
                            applied, this);
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

// ---------- Add-account flow ----------
//
// First sign-in and "Add another account…" share this path. We can't
// reuse the current account's OAuthClient: it's bound to that
// account's keychain slot, so a fresh authorize() would clobber the
// existing tokens. Instead we mint a transient unbound stack
// (OAuthClient + RestClient + GmailClient + SyncService), run the
// OAuth dance against it, and on profileFetched mint the accounts
// row + copy the just-issued tokens onto the new AccountContext's
// bound OAuthClient. Then we tear down the transient stack.

void MainWindow::beginAddAccountFlow() {
    if (pendingAuth_) {
        statusBar()->showMessage(
            tr("Sign-in already in progress."), 4000);
        return;
    }

    auto* config     = config_;
    auto* tokenStore = accounts_->tokenStore();
    if (!tokenStore) {
        QMessageBox::warning(this, tr("Sign-in unavailable"),
            tr("AccountManager was constructed without a TokenStore; "
               "rebuild with a config + token store and try again."));
        return;
    }

    pendingAuth_  = new fc::auth::OAuthClient(config, tokenStore,
                                               /*accountId=*/QString(), this);
    pendingRest_  = new fc::api::RestClient(pendingAuth_, this);
    pendingGmail_ = new fc::api::GmailClient(pendingRest_, this);

    // browserAuthRequested → small dialog so the user can paste the
    // URL if the auto-launch fails. The dialog deletes itself on
    // close; we hold a QPointer so we can flip it to "Signed in" once
    // granted fires (or close it if the flow fails).
    connect(pendingAuth_, &fc::auth::OAuthClient::browserAuthRequested,
            this, [this](const QUrl& url, bool /*openedAutomatically*/) {
        QApplication::clipboard()->setText(url.toString());

        auto* dlg = new QDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle(tr("Sign in with Google"));
        dlg->resize(640, 240);

        auto* layout = new QVBoxLayout(dlg);
        const QString headline = tr(
            "<p>To finish signing in, open the URL below in your "
            "browser. <b>Click <i>Open in Browser</i></b>, or copy the "
            "URL and paste it into any browser yourself.</p>");
        const QString footnote = tr(
            "<p>The URL is already on your clipboard. FirstContact is "
            "listening on a local port — once you complete consent, "
            "the redirect will land here and this dialog will switch "
            "to a sign-in-complete confirmation.</p>");

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
            fc::util::launchBrowser(url);
        });
        connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);

        pendingDlg_ = dlg;
        dlg->show();
    });

    connect(pendingAuth_, &fc::auth::OAuthClient::granted,
            this, [this] {
        qInfo("MainWindow: pendingAuth granted, kicking getProfile");
        if (!pendingGmail_) {
            qWarning("MainWindow: granted fired but pendingGmail_ is null "
                     "(teardown race?)");
            return;
        }
        QPointer<MainWindow> self(this);
        pendingGmail_->getProfile(
            [self](fc::api::GmailClient::Profile p, fc::api::ApiError err) {
            if (!self) {
                qWarning("MainWindow: getProfile callback after destruction");
                return;
            }
            if (err) {
                qWarning("MainWindow: getProfile failed: %s",
                         qUtf8Printable(err.message));
                QMessageBox::warning(self, self->tr("Sign-in failed"),
                    self->tr("Sign-in succeeded but the profile lookup "
                             "failed: %1").arg(err.message));
                if (self->pendingDlg_) self->pendingDlg_->close();
                self->teardownPendingAddAccountFlow();
                return;
            }
            qInfo("MainWindow: getProfile returned email='%s'",
                  qUtf8Printable(p.emailAddress));
            self->finalizePendingAddAccountFlow(p.emailAddress);
        });
    });

    connect(pendingAuth_, &fc::auth::OAuthClient::failed,
            this, [this](const QString& reason) {
        if (pendingDlg_) pendingDlg_->close();
        QMessageBox::warning(this, tr("Sign-in failed"), reason);
        teardownPendingAddAccountFlow();
    });

    statusBar()->showMessage(
        tr("Starting OAuth flow — opening your browser…"), 0);
    pendingAuth_->authorize();
}

void MainWindow::finalizePendingAddAccountFlow(const QString& email) {
    qInfo("MainWindow: finalize entry email='%s', pendingAuth=%p",
          qUtf8Printable(email), static_cast<void*>(pendingAuth_));
    if (!pendingAuth_) return;   // teardown raced us

    // Mint the accounts row (idempotent on email — re-running OAuth
    // for an already-signed-in address returns the existing id).
    const QString id = accounts_->add(email);
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("Sign-in failed"),
            tr("Couldn't add account row for %1.").arg(email));
        if (pendingDlg_) pendingDlg_->close();
        teardownPendingAddAccountFlow();
        return;
    }

    // Build (or fetch) the per-account stack and seed it with the
    // tokens we just minted on the transient client. adoptTokens
    // persists synchronously to the bound slot, so the new context
    // is immediately authorized.
    auto* ctx = accounts_->ensureContext(id);
    if (ctx && ctx->auth() && pendingAuth_) {
        // ctx->auth() lives with its AccountContext on the UI thread. Queue
        // these token mutations so adoption settles after the current
        // profile callback; tokensLoaded flowing through AccountManager
        // drives the post-adoption UI refresh.
        auto* a = ctx->auth();
        const auto snap = pendingAuth_->tokensSnapshot();
        postToObject(a, [a, snap, email] {
            a->adoptTokens(snap);
            a->setAccountEmail(email);
        });
    }

    // Make the new account current. The currentAccountChanged hook
    // wires sidebar / list / reader to the new account_id.
    accounts_->setCurrentAccountId(id);

    if (pendingDlg_) {
        pendingDlg_->setWindowTitle(tr("Sign in with Google — done"));
        pendingDlg_->close();
    }
    statusBar()->showMessage(
        tr("Sign-in successful — %1").arg(email), 6000);

    // Kick the new context's sync onto its scheduler.
    if (ctx && ctx->sync()) {
        auto* s = ctx->sync();
        qInfo("MainWindow: kicking initial sync ctx=%p sync=%p "
              "ctx->accountId='%s' (target id='%s')",
              static_cast<void*>(ctx),
              static_cast<void*>(s),
              qUtf8Printable(ctx->accountId()),
              qUtf8Printable(id));
        // Queue sync startup so runOnce/startScheduler settle after the
        // new account has become current.
        postToObject(s, [s] { s->runOnce(); s->startScheduler(); });
    } else {
        qWarning("MainWindow: no ctx/sync to kick after Add-account "
                 "(ctx=%p)", static_cast<void*>(ctx));
    }
    if (outbox_)  outbox_->start();
    if (pending_) pending_->start();
    if (drafts_)  drafts_->start();

    teardownPendingAddAccountFlow();
    refreshAccountIndicator();
    refreshAccountMenu();
}

void MainWindow::teardownPendingAddAccountFlow() {
    if (pendingGmail_) { pendingGmail_->deleteLater(); pendingGmail_ = nullptr; }
    if (pendingRest_)  { pendingRest_->deleteLater();  pendingRest_  = nullptr; }
    if (pendingAuth_)  { pendingAuth_->deleteLater();  pendingAuth_  = nullptr; }
    pendingDlg_ = nullptr;   // QDialog::WA_DeleteOnClose handles teardown
}

}  // namespace fc::ui
