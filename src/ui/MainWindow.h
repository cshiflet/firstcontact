#pragma once

#include "models/Message.h"

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

class QAction;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QSplitter;
class QToolButton;

namespace fc::ui { class SpinningToolButton; }

namespace fc { class MessageListModel; }
namespace fc::auth  { class OAuthClient; class ClientConfig; }
namespace fc::api   { class GmailClient; class RestClient; }
class QDialog;
namespace fc::sync  { class SyncService; class OutboxWorker;
                      class PendingOpsWorker; class DraftSync; }
namespace fc::cache { class BodyCompressionWorker; }
namespace fc::account { class AccountManager; }

namespace fc::ui {

class SidebarWidget;
class MessageListView;
class ReaderPane;
class TrayController;
class Shortcuts;
class ComposeWindow;

// Top-level shell. Owns no services — it gets handed pointers to the
// Bootstrap-managed singletons. All long-lived state lives in cache repos.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(fc::auth::ClientConfig* config,
               fc::auth::OAuthClient* auth,
               fc::api::GmailClient* gmail,
               fc::sync::SyncService* sync,
               fc::sync::OutboxWorker* outbox,
               fc::sync::PendingOpsWorker* pending,
               fc::sync::DraftSync* drafts,
               fc::account::AccountManager* accounts,
               QWidget* parent = nullptr);

private slots:
    void onSignIn();
    void onSignOut();
    // Multi-account variant: signs the named account out and pops a
    // "drop cache?" yes/no/cancel prompt. yes wipes the cache rows
    // for that accountId; no leaves them in place (re-sign-in
    // resumes without re-syncing); cancel aborts the whole flow.
    void onSignOutAccount(const QString& accountId);
    void onRefresh();
    void onLabelSelected(const QString& accountId, const QString& labelId);
    void onMessageActivated(const QString& messageId, int row);
    void onSearchChanged();
    void onSearchSubmit();
    void onComposeNew();
    void onReplyCurrent();
    void onReplyAllCurrent();
    void onForwardCurrent();
    void onCreateLabel(const QString& accountId, const QString& parentLabelId);
    void onRenameLabel(const QString& accountId, const QString& labelId);
    void onDeleteLabel(const QString& accountId, const QString& labelId);
    void onOpenSettings();
    void onToggleStar();
    void onToggleStarFor(const QString& messageId);
    void onArchiveCurrent();
    void onDeleteCurrent();
    // Toggle read / unread for the entire current thread. If any
    // message in the thread carries UNREAD we drop UNREAD from every
    // message; otherwise we add UNREAD to every message.
    void onToggleReadCurrent();
    // Explicit Gmail-web bindings: Shift+I always marks the current
    // thread as read, Shift+U always marks it unread (no toggle).
    void onMarkReadCurrent();
    void onMarkUnreadCurrent();
    // Gmail-web `u`: return focus to the threadlist. Doesn't change
    // selection or read state — just yanks keyboard focus away from
    // whatever child widget had it (typically the reader pane).
    void onBackToList();
    // Gmail-web `?`: show a proper grouped keyboard shortcuts dialog.
    void onShowShortcutsHelp();
    // Gmail-web `o` / Enter: open the currently-selected message.
    void onOpenCurrent();
    // Gmail-web `[` / `]`: archive the current conversation, then move
    // selection to the prev / next row in the threadlist.
    void onArchiveAndPrev();
    void onArchiveAndNext();
    // Gmail-web `m`: mute the current thread (apply MUTE label, then
    // archive). Gmail's server-side filtering doesn't apply to our
    // cache, but the label stamp matches Gmail's data shape so the
    // user can find it under "Muted" and unmute on the server.
    void onMuteThread();
    // Gmail-web `!`: move conversation to Spam. Adds SPAM, drops INBOX.
    void onReportSpam();
    // Gmail-web `=` / `-`: toggle IMPORTANT for every message in the
    // current thread.
    void onMarkImportant();
    void onMarkNotImportant();
    // Gmail-web `g i / s / t / d`: jump the sidebar selection to a
    // system label without leaving the keyboard.
    void onGoToLabel(const QString& labelId);

    // First-time "Compress DB?" prompt. Fires once per process per
    // account when the cached-body count first crosses the
    // dictionary-training threshold (currently 200). Pre-filtered
    // by Preferences::dbCompression / dbCompressionPromptShown.
    void onCompressionPromptDue(const QString& accountId, int bodyCount);

    // Settings → "Recompress" button. Walks the user through a
    // confirmation showing how many rows will be processed, then
    // spawns a BodyCompressionWorker in Recompress mode.
    void onRecompressRequested(const QString& accountId);

    // Helper: spawn a worker, wire its signals to status-bar
    // progress, and stash the QPointer in compressionWorkers_ so a
    // duplicate trigger is gated out. Mode controls whether existing
    // compressed rows are also rewritten.
    void startCompressionWorker(const QString& accountId, int modeInt);
    // Shift+L — flip Preferences::linkDisplayMode and re-render the
    // active reader content so the change is immediately visible.
    void onToggleLinkDisplay();
    // Gmail-web `l` (Apply labels…): pop the LabelChooserDialog in
    // multi-select mode pre-filled with the thread's current label
    // set; on accept, push the diff through applyLabelDiffToThread.
    void onApplyLabelsCurrent();
    // Gmail-web `v` (Move to label…): single-select picker; on accept,
    // add the chosen label and drop the active sidebar label (so the
    // conversation moves rather than gains a label).
    void onMoveToLabelCurrent();
    // Snooze the current thread: pop a time-picker, drop INBOX, stamp
    // every message's snooze_until. A periodic wake-up timer in
    // MainWindow re-applies INBOX once the snooze window lapses.
    void onSnoozeCurrent();
    void wakeDueSnoozedMessages();
    // Left-click: write to a per-session temp dir and hand off to the
    // OS viewer; no permanent save, no AttachmentRepository update.
    void onOpenAttachment(const QString& messageId,
                          const QString& attachmentId,
                          const QString& filename);
    // Right-click → Save as…: always shows a file picker seeded with
    // Preferences::attachmentDir().
    void onSaveAsAttachment(const QString& messageId,
                            const QString& attachmentId,
                            const QString& filename);
    void onDownloadAllAttachments(const QString& messageId);

    // Per-message handlers triggered by the per-card action row. Unlike
    // their *Current siblings (which operate on the focused message and
    // typically apply thread-wide), these target the SPECIFIC messageId
    // and apply label diffs to that one message only — Gmail-web's per-
    // card "Delete this message" / "Mark unread" semantics.
    void onReplyToMessage(const QString& messageId);
    void onReplyAllToMessage(const QString& messageId);
    void onForwardMessage(const QString& messageId);
    void onArchiveMessage(const QString& messageId);
    void onMarkMessageRead(const QString& messageId, bool read);
    void onDeleteMessage(const QString& messageId);
    void onSnoozeMessage(const QString& messageId);

    void reloadCurrentLabel();
    void reloadSidebar();

    // Resets every cache-driven UI surface (message list, sidebar tree,
    // reader pane, error banner) so a signed-out window doesn't keep
    // rendering the previous account's data straight from cache.
    void clearAccountUiState();

    // Checks whether any per-account OAuthClient is authorized. If
    // none are, calls clearAccountUiState(). Wired to tokensLoaded
    // so the gate re-evaluates after async keychain hydration.
    void enforceActiveAccountGate();

    void onNewMessages(int count);
    // Multi-account notify path: routes to Notifier with the
    // account's email + per-account notification_mode pref. Wired to
    // AccountManager::newMessages.
    void onNewMessagesForAccount(const QString& accountId, int count);

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void buildToolBar();
    void buildLayout();
    void wireSignals();
    // Recomputes the "Loading more messages…" / "No more messages"
    // footer text and visibility from the current model + top-up
    // state. Cheap — call from any place that touches those.
    void refreshListFooter();
    void refreshAccountIndicator();
    void refreshAccountMenu();    // (re)builds the Account toolbar dropdown
    void refreshToolbarIcons();   // re-bake icons from the active palette
    void refreshToolbarStyle();   // toggle text-beside-icon / icon-only
    void updateToolbarOverflow(); // hide/show toolbar items into hamburger
    void onSwitchAccount();       // kick OAuth flow to mint a fresh
                                  // accounts row (no sign-out of existing)

    // Adds a brand-new account: spins up a transient unbound stack
    // (OAuthClient + RestClient + GmailClient + SyncService), runs
    // the browser-based OAuth dance, and on success mints the
    // accounts row, copies tokens onto the new AccountContext's
    // bound OAuthClient, and tears the transient stack down.
    void beginAddAccountFlow();
    // Once the transient stack has produced an email (via getProfile
    // after granted), mint the accounts row, copy tokens onto the
    // bound AccountContext, repaint, and tear the transient stack
    // down.
    void finalizePendingAddAccountFlow(const QString& email);
    void teardownPendingAddAccountFlow();
    void openComposeWindow(const fc::Message* parent, int mode);  // mode = ComposeWindow::Mode

    // Returns the GmailClient owned by the active account's
    // AccountContext, or the legacy anonymous gmail_ if no context
    // is active. Use this for any per-thread / per-message API call
    // — gmail_ is the no-tokens Bootstrap fallback and would fail
    // with "no token" on every request.
    fc::api::GmailClient* activeGmail() const;

    // Apply (add, remove) label sets to every message in a thread,
    // queue the equivalent server reconciliation for each, and refresh
    // the visible inbox. Used by archive, mark-read toggle, and the
    // auto-mark-on-open flow.
    void applyLabelDiffToThread(const QString& threadId,
                                 const QStringList& add,
                                 const QStringList& remove);

    // Common shape for the toolbar / shortcut handlers that apply
    // a label diff to the current thread:
    //   1. bail if no thread is selected
    //   2. honour FC_DRY_RUN with a transient status-bar message
    //   3. push the diff through applyLabelDiffToThread
    //   4. show a success status, refresh the list view (and
    //      optionally the sidebar's unread counts).
    // Returns true if the action ran (false on empty-current /
    // dry-run skip), so the caller can apply any local-state
    // tweaks (e.g. flipping currentMessage_.isUnread) only when
    // the diff actually went through.
    bool guardedThreadAction(const QString& dryRunKey,
                              const QString& blockedStatus,
                              const QString& successStatus,
                              const QStringList& add,
                              const QStringList& remove,
                              bool refreshSidebar = false);

    fc::auth::ClientConfig*    config_;
    fc::auth::OAuthClient*     auth_;
    fc::api::GmailClient*      gmail_;
    fc::sync::SyncService*     sync_;
    fc::sync::OutboxWorker*    outbox_;
    fc::sync::PendingOpsWorker* pending_;
    fc::sync::DraftSync*       drafts_;
    fc::account::AccountManager* accounts_;

    QSplitter*               splitter_;
    SidebarWidget*           sidebar_;
    MessageListView*         list_;
    ReaderPane*              reader_;
    fc::MessageListModel*    listModel_;
    // Loading footer below the message list. Three states:
    //   "" / hidden       — idle (more cache rows can be scrolled to)
    //   "Loading more…"    — a server top-up is in flight for the
    //                        currently-visible label
    //   "No more messages" — cache is fully drained AND the server
    //                        reported no more pages, OR the visible
    //                        label is one of the seed labels that
    //                        incremental sync keeps complete.
    // Legacy widget pointer — replaced by an in-list placeholder row
    // driven through MessageListModel::setFooterState. Kept as nullptr
    // for now; remove the field outright once any external
    // friend / accessor stops referencing it.
    QLabel*                  listFooter_       = nullptr;
    // Counter rather than bool so chained top-ups (resumeAfterTopUp
    // triggers cacheExhausted which posts another topUpLabel before
    // the previous topUpFinished's tail-half runs) don't clobber each
    // other's "in flight" flag and leak the footer into None mid-sync.
    int                      topUpsInFlight_   = 0;

    // Per-account in-flight compression worker. Set when the user
    // accepts the "Compress DB?" dialog (or hits Recompress in
    // Settings); cleared on finished/failed. Used to gate further
    // starts and to suppress duplicate prompts. The pointer's
    // lifetime is managed by the worker itself (deletes on thread
    // exit), so MainWindow doesn't need to delete it.
    QHash<QString, QPointer<fc::cache::BodyCompressionWorker>>
                              compressionWorkers_;
    QHash<QString, bool>     serverExhaustedByLabel_;

    // Per-label scroll memory: keyed by `<accountId>::<labelId>` so
    // switching folders restores the user's last-viewed message
    // position. Cross-account ("All Inboxes") gets its own key so it
    // doesn't share state with INBOX of any single account. Saved
    // on outbound label switch, restored on inbound.
    struct LabelScrollState {
        QString messageId;       // currently selected/highlighted, if any
        int     verticalOffset = 0;
        // Loaded row count when the user last left this label.
        // Restored on return so progressive loading isn't redone — a
        // 5000-row Sent folder doesn't get clipped back to the
        // pageSize default of 100 just because the user clicked away
        // and clicked back.
        int     loadedRowCount = 0;
    };
    QHash<QString, LabelScrollState> labelScrollMemory_;
    QString labelScrollKey(const QString& labelId,
                           bool crossAccount) const;
    void saveLabelScrollState();
    void restoreLabelScrollState(const QString& labelId,
                                 bool crossAccount);

    QLineEdit*               searchEdit_;
    QAction*                 searchIconAction_ = nullptr;
    QList<QPair<QAction*, QString>> iconActions_;
    QToolButton*             accountButton_ = nullptr;
    fc::ui::SpinningToolButton* syncBtn_     = nullptr;   // refresh button — spins during sync
    QMenu*                   accountMenu_   = nullptr;

    // Sync indicator state. We drive the main status-bar message
    // slot directly: "Syncing…" while running, "Syncing… Done"
    // for 30 seconds after success, "Sync failed" for 5 seconds
    // after a failure, and "Signed in as <email>" the rest of the
    // time. messageChanged() is hooked so transient messages from
    // other surfaces (Archived, Saved to …, etc.) auto-revert to
    // the appropriate baseline once they expire.
    bool                     isSyncing_      = false;
    bool                     lastSyncFailed_ = false;

    // Persistent error chip. Shown in the status bar (right-side
    // permanent slot) whenever we have an unresolved sync / outbox
    // failure that the user should be looking at. Includes a
    // dismiss button so the user can clear it; we also auto-clear
    // when the next sync starts so a subsequent successful run
    // tidies up after itself.
    QFrame*                  errorBanner_       = nullptr;
    QLabel*                  errorBannerLabel_  = nullptr;

    // "↓ 1.2 MB" pill on the status bar. Tooltip explains: down / up /
    // request count since launch. Bumped via SessionTransfer::changed
    // — coalesced through the Qt event queue so we don't repaint per
    // byte.
    QLabel*                  bandwidthLabel_    = nullptr;
    void                     refreshBandwidthLabel();

    // Overflow plumbing — hamburger button + menu plus the ordered list of
    // toolbar entries that may collapse into the menu when the window is
    // too narrow. updateToolbarOverflow() walks the list and decides
    // (per resize) which entries stay on the toolbar vs. live in the menu.
    QToolBar*                toolBar_           = nullptr;
    QToolButton*             overflowButton_    = nullptr;
    QAction*                 overflowAction_    = nullptr;
    QMenu*                   overflowMenu_      = nullptr;
    struct OverflowEntry {
        QAction* action;     // the toolbar action being managed
        QAction* toolbarBefore = nullptr;  // anchor for re-insertion
        QString  text;       // menu label when collapsed
        int      priority;   // lower = collapse sooner
        // Optional custom invocation when this entry is exposed via the
        // hamburger menu. Used for the search bar — its toolbar
        // representation is a QLineEdit widget (not a triggerable QAction)
        // so the menu proxy needs to do something app-specific (open a
        // small input dialog) instead of a generic action->trigger().
        std::function<void()> menuTrigger;
        QString  menuIconName;   // resource name for the menu icon (optional)
    };
    std::vector<OverflowEntry> overflowEntries_;
    TrayController*          tray_;
    Shortcuts*               shortcuts_;

    QString                  currentLabelId_;
    QString                  currentSearchQuery_;
    fc::Message              currentMessage_;
    int                      currentRow_ = -1;

    // Transient stack used by Add-account / first-sign-in. Lives only
    // for the duration of one OAuth dance; teardownPendingAddAccountFlow
    // deletes every member when the flow either succeeds (tokens
    // copied onto the new AccountContext) or fails. We don't include
    // a transient SyncService — SyncService::runOnce bails on an
    // empty accountId, and we just need one getProfile to learn the
    // email so we can mint the accounts row.
    fc::auth::OAuthClient*   pendingAuth_   = nullptr;
    fc::api::RestClient*     pendingRest_   = nullptr;
    fc::api::GmailClient*    pendingGmail_  = nullptr;
    QDialog*                 pendingDlg_    = nullptr;

    // Multi-account: id of the account whose data the three panes
    // currently display. Seeded from AccountManager::currentAccountId
    // at construction; replaced on user action via the toolbar account
    // menu and the AccountManagerDialog. Empty before sign-in / after
    // the last account is removed.
    QString                  currentAccountId_;

    // v2: when the user clicks "All Inboxes" in the sidebar, the
    // message list switches to a cross-account view. Selecting any
    // per-account label flips it back to per-account mode and pins
    // currentAccountId_.
    bool                     crossAccountView_ = false;
};

}  // namespace fc::ui
