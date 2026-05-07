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

int currentSchemaVersion(QSqlDatabase& db) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='meta'"))) {
        return 0;
    }
    if (!q.next()) return 0;
    q.prepare(QStringLiteral(
        "SELECT value FROM meta WHERE key = 'schema_version'"));
    if (!q.exec() || !q.next()) return 0;
    return q.value(0).toInt();
}

QString readResource(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qFatal("missing migration resource: %s", qUtf8Printable(path));
    }
    return QString::fromUtf8(f.readAll());
}

QStringList splitStatements(const QString& sql) {
    // Split on ';' boundaries while respecting BEGIN/END blocks for triggers.
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
            qFatal("migration failed: %s\nSQL: %s",
                   qUtf8Printable(q.lastError().text()),
                   qUtf8Printable(stmt));
        }
    }
}

// Same as execAll, but runs the whole batch inside a single transaction
// with foreign-key enforcement temporarily disabled — required for the
// v6 multi-account composite-FK rebuild, where each per-account table has
// to be dropped + recreated and the in-flight intermediate states would
// otherwise trip the FK checker against still-pointing-at-old-schema
// child tables. PRAGMA foreign_keys must be set OUTSIDE a transaction
// (sqlite ignores it inside), so we toggle it before/after the txn.
//
// Migrations 0001-0005 use plain execAll; only 0006 needs this path.
void execAllInTransaction(QSqlDatabase& db, const QString& sql) {
    {
        QSqlQuery pragmaOff(db);
        if (!pragmaOff.exec(QStringLiteral("PRAGMA foreign_keys = OFF"))) {
            qFatal("migration: failed to disable foreign keys: %s",
                   qUtf8Printable(pragmaOff.lastError().text()));
        }
    }
    if (!db.transaction()) {
        qFatal("migration: failed to BEGIN transaction: %s",
               qUtf8Printable(db.lastError().text()));
    }
    for (const QString& stmt : splitStatements(sql)) {
        QSqlQuery q(db);
        if (!q.exec(stmt)) {
            const QString msg = q.lastError().text();
            db.rollback();
            QSqlQuery pragmaOn(db);
            pragmaOn.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
            qFatal("migration failed: %s\nSQL: %s",
                   qUtf8Printable(msg),
                   qUtf8Printable(stmt));
        }
    }
    if (!db.commit()) {
        qFatal("migration: failed to COMMIT transaction: %s",
               qUtf8Printable(db.lastError().text()));
    }
    {
        QSqlQuery pragmaOn(db);
        if (!pragmaOn.exec(QStringLiteral("PRAGMA foreign_keys = ON"))) {
            qWarning("migration: failed to re-enable foreign keys: %s",
                     qUtf8Printable(pragmaOn.lastError().text()));
        }
    }
}

void setSchemaVersion(QSqlDatabase& db, int v) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO meta(key, value) VALUES ('schema_version', :v) "
        "ON CONFLICT(key) DO UPDATE SET value = :v"));
    q.bindValue(QStringLiteral(":v"), QString::number(v));
    q.exec();
}

}  // namespace

void Migrations::run(QSqlDatabase& db) {
    const int v = currentSchemaVersion(db);
    if (v < 1) {
        execAll(db, readResource(QStringLiteral(":/schema/0001_init.sql")));
        setSchemaVersion(db, 1);
    }
    if (v < 2) {
        execAll(db, readResource(QStringLiteral(":/schema/0002_fts.sql")));
        setSchemaVersion(db, 2);
    }
    if (v < 3) {
        execAll(db, readResource(QStringLiteral(":/schema/0003_body_html.sql")));
        setSchemaVersion(db, 3);
    }
    if (v < 4) {
        execAll(db, readResource(QStringLiteral(":/schema/0004_send_at.sql")));
        setSchemaVersion(db, 4);
    }
    if (v < 5) {
        execAll(db, readResource(QStringLiteral(":/schema/0005_snooze.sql")));
        setSchemaVersion(db, 5);
    }
    if (v < 6) {
        // Multi-account: rebuild every per-account table with a composite
        // (account_id, …) primary key. Has to run in a transaction so the
        // composite-FK dance (drop child, drop parent, rename _new → orig)
        // doesn't leave half-built tables on failure.
        execAllInTransaction(
            db, readResource(QStringLiteral(":/schema/0006_multi_account.sql")));
        setSchemaVersion(db, 6);
    }
}

}  // namespace fc::cache
