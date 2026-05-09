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
// Thread-affinity: lives on the UI thread. The sync threads do not
// touch it directly — they read account_id strings out of cache rows
// and dispatch through the per-account stack picked by the manager.
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
    int clearMessagesOlderThan(const QString& id, int days);

    // Deletes oldest-first messages in the named account until the
    // approximate cache size (cacheSizeFor) drops to `targetBytes`.
    // Returns the number of messages deleted. Useful for the cache
    // manager's "Reduce to <N> MB" action.
    int clearMessagesToTargetSize(const QString& id, qint64 targetBytes);

    // Per-account cache statistics — what cacheSizeFor returns plus
    // counts the dialog wants to display: messages, threads,
    // labels (folders), attachments, drafts, outbox, pending_ops.
    // Populated for an existing account or an orphaned account_id;
    // either way the row counts come straight from the per-account
    // tables.
    struct AccountCacheStats {
        qint64 sizeBytes        = 0;
        int    messageCount     = 0;
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

private:
    void selectInitialCurrent();
    void buildContextsForAllAccounts();

    QList<AccountInfo> accounts_;
    QString            currentId_;

    fc::auth::ClientConfig*  config_     = nullptr;
    fc::auth::TokenStore*    tokenStore_ = nullptr;
    QHash<QString, AccountContext*> contexts_;
};

}  // namespace fc::account
