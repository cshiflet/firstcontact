#include "Database.h"

#include "Migrations.h"
#include "util/Paths.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QThread>

namespace fc::cache {

namespace {

QString perThreadName() {
    // QSqlDatabase keys connections by name; Qt forbids sharing a connection
    // across threads. Append the thread address for uniqueness.
    return QStringLiteral("fc_cache_%1").arg(
        reinterpret_cast<quintptr>(QThread::currentThread()), 0, 16);
}

void applyPragmas(QSqlDatabase& db) {
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    q.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    q.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    q.exec(QStringLiteral("PRAGMA temp_store = MEMORY"));
    q.exec(QStringLiteral("PRAGMA mmap_size = 67108864"));  // 64 MB
}

}  // namespace

QString Database::connectionName() { return perThreadName(); }

QString Database::filePath() { return fc::util::cacheDbPath(); }

QString Database::defaultAccountId() {
    QSqlDatabase db = QSqlDatabase::database(perThreadName(), /*open=*/true);
    if (!db.isOpen()) return {};
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT id FROM accounts "
            "ORDER BY is_default DESC, "
            "         COALESCE(last_used_at, 0) DESC, "
            "         sort_order ASC, email ASC "
            "LIMIT 1"))) {
        return {};
    }
    if (!q.next()) return {};
    return q.value(0).toString();
}

void Database::initialize() {
    const QString name = perThreadName();
    if (QSqlDatabase::contains(name)) return;

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(filePath());
    if (!db.open()) {
        qFatal("Failed to open cache db at %s: %s",
               qUtf8Printable(filePath()),
               qUtf8Printable(db.lastError().text()));
    }
    fc::util::restrictPermissionsToOwner(filePath());
    // SQLite WAL produces a -wal sidecar at runtime; lock it down too once present.
    fc::util::restrictPermissionsToOwner(filePath() + QStringLiteral("-wal"));
    fc::util::restrictPermissionsToOwner(filePath() + QStringLiteral("-shm"));
    applyPragmas(db);
    Migrations::run(db);
}

}  // namespace fc::cache

// Helper kept out of the header to avoid making every TU pull in QSqlDatabase.
#include <QSqlDatabase>
namespace fc::cache {
QSqlDatabase databaseHandle() {
    return QSqlDatabase::database(Database::connectionName(), /*open=*/true);
}
}  // namespace fc::cache
