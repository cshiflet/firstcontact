#pragma once

#include "models/Message.h"

#include <QList>
#include <QMainWindow>
#include <QPair>
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

namespace fc { class MessageListModel; }
namespace fc::auth  { class OAuthClient; class ClientConfig; }
namespace fc::api   { class GmailClient; }
namespace fc::sync  { class SyncService; class OutboxWorker;
                      class PendingOpsWorker; class DraftSync; }

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
               QWidget* parent = nullptr);

private slots:
    void onSignIn();
    void onSignOut();
    void onRefresh();
    void onLabelSelected(const QString& id);
    void onMessageActivated(const QString& messageId, int row);
    void onSearchChanged();
    void onSearchSubmit();
    void onComposeNew();
    void onReplyCurrent();
    void onReplyAllCurrent();
    void onForwardCurrent();
    void onCreateLabel(const QString& parentLabelId);
    void onRenameLabel(const QString& labelId);
    void onDeleteLabel(const QString& labelId);
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

    void reloadCurrentLabel();
    void reloadSidebar();
    void onNewMessages(int count);

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void buildToolBar();
    void buildLayout();
    void wireSignals();
    void refreshAccountIndicator();
    void refreshAccountMenu();    // (re)builds the Account toolbar dropdown
    void refreshToolbarIcons();   // re-bake icons from the active palette
    void refreshToolbarStyle();   // toggle text-beside-icon / icon-only
    void updateToolbarOverflow(); // hide/show toolbar items into hamburger
    void onSwitchAccount();       // sign out + immediately re-authorize
    void openComposeWindow(const fc::Message* parent, int mode);  // mode = ComposeWindow::Mode

    // Apply (add, remove) label sets to every message in a thread,
    // queue the equivalent server reconciliation for each, and refresh
    // the visible inbox. Used by archive, mark-read toggle, and the
    // auto-mark-on-open flow.
    void applyLabelDiffToThread(const QString& threadId,
                                 const QStringList& add,
                                 const QStringList& remove);

    fc::auth::ClientConfig*    config_;
    fc::auth::OAuthClient*     auth_;
    fc::api::GmailClient*      gmail_;
    fc::sync::SyncService*     sync_;
    fc::sync::OutboxWorker*    outbox_;
    fc::sync::PendingOpsWorker* pending_;
    fc::sync::DraftSync*       drafts_;

    QSplitter*               splitter_;
    SidebarWidget*           sidebar_;
    MessageListView*         list_;
    ReaderPane*              reader_;
    fc::MessageListModel*    listModel_;

    QLineEdit*               searchEdit_;
    QAction*                 searchIconAction_ = nullptr;
    QList<QPair<QAction*, QString>> iconActions_;
    QToolButton*             accountButton_ = nullptr;
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
};

}  // namespace fc::ui
