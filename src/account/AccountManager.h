#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

namespace fc::account {

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
    explicit AccountManager(QObject* parent = nullptr);

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

    // Convenience: lookup by id. Returns an empty-id AccountInfo when
    // the id isn't known.
    AccountInfo accountById(const QString& id) const;

signals:
    void accountsChanged();              // accounts list mutated (add/remove/default)
    void currentAccountChanged(const QString& id);
    // Step 6 will fire this after a sync ticks an account-scoped
    // signal; for now it's plumbed but unused.
    void messagesUpdated(const QString& accountId);

private:
    void selectInitialCurrent();

    QList<AccountInfo> accounts_;
    QString            currentId_;
};

}  // namespace fc::account
