#pragma once

#include <QString>

namespace fc::cache {

// Per-thread sqlite handle. Qt requires each thread that touches sqlite to
// open its own QSqlDatabase connection; this class hides that ceremony.
//
// Usage:
//   fc::cache::Database::initialize();              // once on the sync thread
//   QSqlDatabase db = fc::cache::Database::handle(); // returns the per-thread handle
//
// The first call on a given thread opens the connection and runs pending
// migrations. Subsequent calls on the same thread return the cached handle.
class Database {
public:
    static void   initialize();
    static QString connectionName();
    static QString filePath();

    // Returns the active account's id. v1 uses this as the implicit
    // account for repository methods that don't take an accountId. The
    // selection rule is:
    //   1. accounts.is_default = 1, if any
    //   2. otherwise the row with the most recent last_used_at,
    //   3. otherwise the row with the lowest sort_order then email.
    // If the accounts table is empty (e.g. fresh install before first
    // sign-in), returns an empty string. AccountManager (step 4) will
    // own the in-memory current-account selection; until then the
    // legacy zero-arg repository overloads route through this helper.
    static QString defaultAccountId();
};

}  // namespace fc::cache
