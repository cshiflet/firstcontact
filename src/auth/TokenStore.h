#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <functional>

namespace fc::auth {

// Cross-platform secure token storage backed by QtKeychain.
//
// Multi-account: each account's tokens live in their own keychain slot
// keyed by account id. A separate "account-index" slot holds a JSON
// directory of account ids — QtKeychain doesn't expose an enumerate
// API on every platform, so we maintain the index ourselves.
//
// One-time migration: at first construction, if a legacy "primary" slot
// exists, we copy its tokens into a new keyed slot (using either the
// stored email -> AccountManager UUID or a deterministic seed id),
// update the index, and erase "primary". The migration is idempotent.
//
// All operations are async (QtKeychain uses platform IPC under the
// hood). Callbacks fire on the calling thread's event loop.
class TokenStore : public QObject {
    Q_OBJECT
public:
    explicit TokenStore(QObject* parent = nullptr);

    // Per-account record stored in the keychain.
    struct Tokens {
        QString accountId;          // accounts table PK; populated on load
        QString accessToken;
        QString refreshToken;
        qint64  expiresAtUnix = 0;  // seconds since epoch
        QString accountEmail;
        bool valid() const { return !refreshToken.isEmpty(); }
    };

    // Per-account API.
    using LoadOneCb  = std::function<void(bool ok, Tokens, QString error)>;
    using LoadAllCb  = std::function<void(bool ok, QList<Tokens>, QString error)>;
    using DoneCb     = std::function<void(bool ok, QString error)>;

    // Loads a single account's tokens by id.
    void load(const QString& accountId, LoadOneCb cb);

    // Enumerates every keyed slot the index knows about. The legacy
    // "primary" slot, if present, is migrated into a keyed slot and
    // returned in the same callback. Errors short-circuit: a per-slot
    // read failure returns ok=false with the partial list it managed
    // to gather.
    void loadAll(LoadAllCb cb);

    // Saves tokens under their accountId. Overwrites any prior slot
    // for the same id. After a successful write the account-index is
    // updated to include the id (idempotent).
    void save(const Tokens& t, DoneCb cb);

    // Removes the slot for the given account id (and updates the
    // index). The legacy single-account erase() form below routes here
    // via the default account.
    void erase(const QString& accountId, DoneCb cb);

    // Legacy single-account API. Routes to the default account.
    void load(LoadOneCb cb);
    void erase(DoneCb cb);

private:
    static QString serviceName();
    // Per-account slot key. accountId is hex/UUID; we use it verbatim
    // with a stable prefix so test fixtures (and humans peeking at the
    // OS keychain) can recognise the entries.
    static QString slotKey(const QString& accountId);
    // Directory slot — JSON array of known account ids.
    static QString indexKey();
    // Legacy single-account slot, retained for the one-time migration.
    static QString legacyKey();

    void readIndex(std::function<void(QStringList ids,
                                       bool ok, QString error)> cb);
    void writeIndex(const QStringList& ids, DoneCb cb);
    void migrateLegacyIfPresent(std::function<void()> cb);

    bool legacyMigrationAttempted_ = false;
};

}  // namespace fc::auth
