#include "Migrations.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace fc::cache {

namespace {

// Bump on every structural change to resources/schema/schema.sql.
// Existing DBs at any other version are rejected — this is a
// prototype, no migration walk is maintained; users reset via the
// `firstcontact reset-db` CLI command. Starts at 9 to preserve
// continuity with the previous incremental-migration scheme that
// landed here (0001..0009), so pre-existing local DBs keep working
// without a forced reset.
constexpr int kSchemaVersion = 9;

int storedSchemaVersion(QSqlDatabase& db) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='meta'"))) {
        return 0;
    }
    if (!q.next()) return 0;
    if (!q.exec(QStringLiteral(
            "SELECT value FROM meta WHERE key = 'schema_version'"))) {
        return 0;
    }
    if (!q.next()) return 0;
    return q.value(0).toInt();
}

QString readResource(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qFatal("missing schema resource: %s", qUtf8Printable(path));
    }
    return QString::fromUtf8(f.readAll());
}

// Split a SQL script on ';' boundaries, respecting BEGIN/END blocks for
// triggers and skipping line comments. The QSqlDatabase QSQLITE driver
// only accepts one statement per QSqlQuery::exec call.
QStringList splitStatements(const QString& sql) {
    QStringList out;
    QString cur;
    int triggerDepth = 0;
    bool inLineComment = false;
    for (int i = 0; i < sql.size(); ++i) {
        const QChar c = sql[i];
        if (inLineComment) {
            if (c == '\n') inLineComment = false;
            cur += c;
            continue;
        }
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            inLineComment = true;
            cur += c;
            continue;
        }
        const QString upper = sql.mid(i, 6).toUpper();
        if (upper == QLatin1String("BEGIN ") ||
            upper == QLatin1String("BEGIN\n")) ++triggerDepth;
        if (upper.startsWith(QLatin1String("END;")) ||
            upper.startsWith(QLatin1String("END\n")))
            triggerDepth = qMax(0, triggerDepth - 1);

        cur += c;
        if (c == ';' && triggerDepth == 0) {
            const QString trimmed = cur.trimmed();
            if (!trimmed.isEmpty()) out << trimmed;
            cur.clear();
        }
    }
    if (!cur.trimmed().isEmpty()) out << cur.trimmed();
    return out;
}

void execAll(QSqlDatabase& db, const QString& sql) {
    for (const QString& stmt : splitStatements(sql)) {
        QSqlQuery q(db);
        if (!q.exec(stmt)) {
            qFatal("schema init failed: %s\nSQL: %s",
                   qUtf8Printable(q.lastError().text()),
                   qUtf8Printable(stmt));
        }
    }
}

void stampVersion(QSqlDatabase& db, int version) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO meta(key, value) VALUES ('schema_version', :v) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.bindValue(QStringLiteral(":v"), version);
    if (!q.exec()) {
        qFatal("schema init: failed to stamp schema_version: %s",
               qUtf8Printable(q.lastError().text()));
    }
}

}  // namespace

void Migrations::run(QSqlDatabase& db) {
    const int stored = storedSchemaVersion(db);

    if (stored == kSchemaVersion) return;   // already initialized

    if (stored == 0) {
        // Fresh database. Apply the bundled schema in a single
        // transaction and stamp the version.
        if (!db.transaction()) {
            qFatal("schema init: BEGIN failed: %s",
                   qUtf8Printable(db.lastError().text()));
        }
        execAll(db, readResource(QStringLiteral(":/schema/schema.sql")));
        stampVersion(db, kSchemaVersion);
        if (!db.commit()) {
            qFatal("schema init: COMMIT failed: %s",
                   qUtf8Printable(db.lastError().text()));
        }
        return;
    }

    // Pre-existing database at a different version — refuse rather
    // than silently corrupting. This is a prototype; migrations are
    // not maintained.
    qFatal("Cache database at version %d, but this build expects "
           "version %d. No migration available — clear the cache "
           "with `firstcontact reset-db` (auth tokens in QtKeychain "
           "are preserved) and relaunch.",
           stored, kSchemaVersion);
}

}  // namespace fc::cache
