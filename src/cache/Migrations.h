#pragma once

class QSqlDatabase;

namespace fc::cache {

// Migrations are idempotent. The current schema_version lives in the `meta`
// table; SQL files are bundled into the binary via Qt resources and applied
// in order on first connect.
class Migrations {
public:
    static void run(QSqlDatabase& db);
};

QSqlDatabase databaseHandle();   // defined in Database.cpp

}  // namespace fc::cache
