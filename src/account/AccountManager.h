#pragma once

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace fc::auth { class ClientConfig; class TokenStore; }

namespace fc::account {

class AccountContext;

// Public read-only view of an `accounts` table row. The persistent
// state lives in sqlite; AccountManager keeps an in-memory cache
// (refreshed on writes via reload()) so the UI can iterate without
// hitting the DB on every paint.
struct AccountInfo {
    QString id;            // stable UUID, PK in `accounts`
    QString email;         // canonical Gmail address (UNIQUE)
    QString displayName;   // empty until the user sets one
    QString colorHint;     // v3 accent palette key
    int     sortOrder = 0;
    qint64  createdAt = 0;
    qint64  lastUsedAt = 0;
    bool    isDefault = false;
};

// In-memory representation of the signed-in accounts plus the active
// selection. Owns the structure that step 6 will hang per-account
// API clients (OAuthClient, RestClient, GmailClient, SyncService)
// off of via AccountContext.
//
// Thread-affinity: lives on the UI thread. Each AccountContext spawns
// its own QThread on which its OAuthClient / RestClient / GmailClient /
// SyncService live; the per-context signal forwarders below connect
// with AutoConnection, which becomes a queued cross-thread post when
// the signal originates on a sync thread. The sync threads do not
// touch AccountManager itself — they read account_id strings out of
// cache rows and dispatch through the per-account stack picked by the
// manager.
class AccountManager : public QObject {
    Q_OBJECT
public:
    // The basic constructor leaves the manager without per-account
    // API stacks (AccountContext). Step 6 introduces a constructor
    // overload that takes a ClientConfig + TokenStore so the manager
    // can build an AccountContext per accounts row at startup.
    explicit AccountManager(QObject* parent = nullptr);
    AccountManager(fc::auth::ClientConfig* config,
                    fc::auth::TokenStore*   tokenStore,
                    QObject* parent = nullptr);
    ~AccountManager() override;

    // Re-reads the `accounts` table. Cheap; called after every add /
    // remove / setDefault / markUsed and from Bootstrap on launch.
    void reload();

    // All known accounts, sorted (sortOrder, email) — same order the
    // toolbar account menu and AccountManagerDialog render.
    QList<AccountInfo> accounts() const;

    // The currently-active account. v1 sidebar / list / reader paint
    // this account's data. Empty before any account is signed in.
    QString currentAccountId() const;
    void    setCurrentAccountId(const QString& id);

    // Mints a new accounts row (UUID id) and returns its id. Email is
    // UNIQUE — adding an existing email returns the existing id and
    // doesn't insert a duplicate. The caller is responsible for the
    // OAuth flow + TokenStore::save afterwards. Step 5 wires this into
    // the addAccount UX.
    QString add(const QString& email,
                const QString& displayName = {});

    // Cascades deletion of every row in cache (messages, labels,
    // drafts, outbox, pending_ops, account_meta) via the FK ON DELETE
    // CASCADE chain set up in 0006_multi_account.sql. Returns true if
    // a row was removed, false if the id wasn't known.
    bool remove(const QString& id);

    // Promotes the account to default and demotes every other. v1
    // surfaces this as a "Make default" action in
    // AccountManagerDialog (step 9).
    void setDefault(const QString& id);

    // Stamps last_used_at = now() on the given account. Called every
    // time setCurrentAccountId promotes a different account, so that
    // the next launch resumes on the most-recently-active one.
    void markUsed(const QString& id);

    // v3 — per-account accent palette. Eight fixed slots ("blue",
    // "green", "red", "purple", "orange", "teal", "pink", "yellow")
    // mirror Gmail's label palette. Stored verbatim in accounts.color_hint;
    // the UI maps the slug to a QColor via accentColorFor().
    static QStringList accentPalette();
    void setAccentColor(const QString& id, const QString& accentSlug);
    static QColor accentColorFor(const QString& accentSlug);

    // Wipes every cache row scoped to the given account (messages,
    // threads, labels, message_labels, attachments, drafts, outbox,
    // pending_ops, account_meta) without deleting the accounts row
    // itself. v1 surfaces this as the "drop cache?" yes branch in
    // the sign-out prompt; v4 reuses it under the cache-manager
    // dialog. Returns true on success.
    bool dropCache(const QString& id);

    // v4 — cache manager dialog support.

    // Approximate cache size for the given account: sum of
    // messages.bytes_cached plus the sizes of any attachments whose
    // local_path file exists on disk.
    qint64 cacheSizeFor(const QString& id) const;

    // Returns account_ids that appear in cache rows (any per-account
    // table) but no longer have an accounts row. Caused by an out-of-
    // band accounts-table delete that didn't cascade due to an old
    // schema or a manual SQL operation; cache-manager surfaces them
    // as an "Orphaned" group with a "Drop orphaned cache" button.
    QStringList orphanedAccountIds() const;

    // Wipes every cache row whose account_id is in orphanedAccountIds().
    // Returns the number of orphaned account ids cleaned up.
    int dropOrphanedCache();

    // Deletes messages whose internal_date is older than `days` days
    // ago, scoped to the given account. Threads / labels stay; only
    // body+metadata for old messages is purged. Useful for users who
    // want to free disk without losing label structure. Returns the
    // number of deleted message rows.
    //
    // reconcileFts = true (default) rebuilds the FTS5 index after
    // the delete so search results catch up. Auto-prune passes
    // false on the per-axis calls and rebuilds once at the end —
    // saves two redundant full-table rebuilds per messagesUpdated.
    int clearMessagesOlderThan(const QString& id, int days,
                                 bool reconcileFts = true);

    // Deletes oldest-first messages in the named account until the
    // approximate cache size (cacheSizeFor) drops to `targetBytes`.
    // Returns the number of messages deleted. Useful for the cache
    // manager's "Reduce to <N> MB" action.
    int clearMessagesToTargetSize(const QString& id, qint64 targetBytes,
                                    bool reconcileFts = true);

    // Deletes oldest-first messages until the total cached message
    // count for `id` drops to `targetCount`. Returns the number of
    // deleted rows. Used by the auto-prune workflow when the user
    // has set a per-account message-count cap.
    int clearMessagesToTargetCount(const QString& id, int targetCount,
                                     bool reconcileFts = true);

    // Runs all three caps in order (age → count → size). A zero
    // value on any axis means "no cap, skip." Cheap when the cache
    // is already within bounds (COUNT / SELECT queries with no
    // follow-up DELETE). Safe to call after every sync. Returns
    // total messages deleted across the three passes.
    //
    // Reading Preferences for the bound values happens at the
    // caller (typically MainWindow): fc_account can't pull Preferences
    // (lives in fc_ui) without creating a circular dep.
    int applyAutoPruneFor(const QString& id,
                           int maxAgeDays,
                           int maxMessages,
                           int maxCacheMb);

    // Per-account cache statistics — what cacheSizeFor returns plus
    // counts the dialog wants to display: messages, threads,
    // labels (folders), attachments, drafts, outbox, pending_ops.
    // Populated for an existing account or an orphaned account_id;
    // either way the row counts come straight from the per-account
    // tables.
    struct AccountCacheStats {
        qint64 sizeBytes        = 0;
        int    messageCount     = 0;
        int    bodyCount        = 0;   // subset of messageCount with cached body
        int    threadCount      = 0;
        int    labelCount       = 0;
        int    attachmentCount  = 0;
        int    draftCount       = 0;
        int    outboxCount      = 0;
        int    pendingOpsCount  = 0;
    };
    AccountCacheStats statsFor(const QString& id) const;

    // Convenience: lookup by id. Returns an empty-id AccountInfo when
    // the id isn't known.
    AccountInfo accountById(const QString& id) const;

    // Step 6: per-account stack lookup. Returns nullptr if the manager
    // was constructed without a config/tokenStore, or if the id has no
    // context (account not yet signed in).
    AccountContext* contextFor(const QString& id) const;
    AccountContext* currentContext() const;
    // List every active context. Used by MainWindow to start one
    // sync scheduler per signed-in account, and by the workers.
    QList<AccountContext*> allContexts() const;

    // The shared TokenStore + ClientConfig that every per-account
    // OAuthClient was constructed against. Exposed so the Add-account
    // flow can mint a transient unbound OAuthClient against the same
    // keychain backing — see MainWindow::beginAddAccountFlow.
    fc::auth::TokenStore*  tokenStore() const { return tokenStore_; }
    fc::auth::ClientConfig* clientConfig() const { return config_; }

    // Wires (or rebuilds) an AccountContext for the given account id.
    // Idempotent: a second call returns the same context. Returns
    // nullptr if the manager wasn't given a config/tokenStore at
    // construction.
    AccountContext* ensureContext(const QString& id);

signals:
    void accountsChanged();              // accounts list mutated (add/remove/default)
    void currentAccountChanged(const QString& id);
    // Cross-account aggregated sync signals. Each per-account
    // SyncService emits its own signals; AccountManager re-emits them
    // tagged with the source accountId so MainWindow can react
    // appropriately (e.g. only repaint the message list when the
    // current account is the one that ticked).
    void labelsUpdated(const QString& accountId);
    void messagesUpdated(const QString& accountId);
    void newMessages(const QString& accountId, int count);
    void syncFailed(const QString& accountId, const QString& reason);
    void topUpStarted(const QString& accountId, const QString& labelId);
    void topUpFinished(const QString& accountId, const QString& labelId,
                        int newRowsStored, bool serverExhausted);
    // Re-emitted from SyncService::compressionPromptDue. MainWindow
    // shows the first-time "Compress DB?" dialog when this fires.
    void compressionPromptDue(const QString& accountId, int bodyCount);
    // Re-emitted from SyncService::cacheLabel{Progress,Finished} so
    // MainWindow can drive a status-bar readout for the per-label
    // "Cache all messages" right-click action.
    void cacheLabelProgress(const QString& accountId,
                              const QString& labelId, int totalStored);
    void cacheLabelFinished(const QString& accountId,
                              const QString& labelId, int totalStored,
                              bool cancelled);
    // Coarser-grained "any sync is active for this account" pair.
    // Fires when the per-account SyncService leaves Idle (initial
    // sync, incremental sync, or top-up) and again when it returns
    // to Idle. MainWindow uses these to drive the status-bar /
    // placeholder "Syncing…" feedback for paths the per-label
    // topUpStarted/Finished pair doesn't cover (notably the initial
    // sync after a cache wipe).
    void syncStarted(const QString& accountId);
    void syncFinished(const QString& accountId);
    // Forwarded from the per-account OAuthClient::tokensLoaded.
    // MainWindow uses this to trigger a sidebar/list reload after
    // async keychain hydration completes (or after Add-account
    // adopts tokens onto a freshly-built context).
    void tokensLoaded(const QString& accountId);
    // Forwarded from the per-account OAuthClient::signedOut.
    // AccountManagerDialog and the toolbar indicator listen so the
    // signed-out row disappears (or flips to "Signed out") without
    // waiting for the dialog to be re-opened.
    void accountSignedOut(const QString& accountId);

    // Fired right after dropCache successfully wipes a per-account
    // cache. MainWindow uses this to repaint the sidebar / message
    // list (which would otherwise still show the now-gone rows
    // until the next sync emits messagesUpdated) and to nudge a
    // fresh runOnce so the empty cache doesn't sit there until the
    // 60s scheduler tick.
    void cacheCleared(const QString& accountId);

private:
    void selectInitialCurrent();
    void buildContextsForAllAccounts();
    // Re-emits every per-account SyncService + OAuthClient signal
    // through this manager, tagged with `aid`. Called from
    // buildContextsForAllAccounts (startup) and ensureContext
    // (Add-account flow) — keeping the forwarding in one helper so
    // a new per-account signal doesn't need to land in two places.
    void wireContextForwarding(AccountContext* ctx, const QString& aid);

    QList<AccountInfo> accounts_;
    QString            currentId_;

    fc::auth::ClientConfig*  config_     = nullptr;
    fc::auth::TokenStore*    tokenStore_ = nullptr;
    QHash<QString, AccountContext*> contexts_;
};

}  // namespace fc::account
