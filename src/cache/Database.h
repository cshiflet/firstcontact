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
};

}  // namespace fc::cache
