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

void setSchemaVersion(QSqlDatabase& db, int v) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO meta(key, value) VALUES ('schema_version', :v) "
        "ON CONFLICT(key) DO UPDATE SET value = :v"));
    q.bindValue(QStringLiteral(":v"), QString::number(v));
    if (!q.exec()) {
        // If we can't update the schema version, the next run will
        // think this migration never happened and will replay every
        // statement on top of an already-migrated DB. That's worse
        // than aborting outright.
        qFatal("Migrations: failed to update schema_version: %s",
               qUtf8Printable(q.lastError().text()));
    }
}

// Runs `body` inside a single sqlite transaction. Any qFatal coming
// out of execAll / setSchemaVersion aborts the process before the
// commit, so the half-applied step is rolled back automatically when
// the WAL replay runs on the next launch. Without the transaction
// boundary a partially-applied schema would persist on disk and
// every subsequent migration would compound the breakage.
template <typename Body>
void runStep(QSqlDatabase& db, Body body) {
    if (!db.transaction()) {
        qFatal("Migrations: failed to start transaction: %s",
               qUtf8Printable(db.lastError().text()));
    }
    body();
    if (!db.commit()) {
        // Best-effort rollback before bailing — sqlite may still
        // have the open transaction; commit failure is rare but
        // worth surfacing rather than silently losing the step.
        db.rollback();
        qFatal("Migrations: failed to commit step: %s",
               qUtf8Printable(db.lastError().text()));
    }
}

}  // namespace

void Migrations::run(QSqlDatabase& db) {
    // Connection-level PRAGMAs first — these can't run inside a
    // transaction (journal_mode in particular), so we apply them
    // directly before any version-checked step. Idempotent on a
    // re-opened database, so running every launch is fine.
    {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("PRAGMA journal_mode = WAL"))) {
            qWarning("Migrations: PRAGMA journal_mode failed: %s",
                     qUtf8Printable(q.lastError().text()));
        }
        if (!q.exec(QStringLiteral("PRAGMA foreign_keys = ON"))) {
            qWarning("Migrations: PRAGMA foreign_keys failed: %s",
                     qUtf8Printable(q.lastError().text()));
        }
    }

    const int v = currentSchemaVersion(db);
    if (v < 1) {
        runStep(db, [&] {
            execAll(db, readResource(QStringLiteral(":/schema/0001_init.sql")));
            setSchemaVersion(db, 1);
        });
    }
    if (v < 2) {
        runStep(db, [&] {
            execAll(db, readResource(QStringLiteral(":/schema/0002_fts.sql")));
            setSchemaVersion(db, 2);
        });
    }
    if (v < 3) {
        runStep(db, [&] {
            execAll(db, readResource(QStringLiteral(":/schema/0003_body_html.sql")));
            setSchemaVersion(db, 3);
        });
    }
    if (v < 4) {
        runStep(db, [&] {
            execAll(db, readResource(QStringLiteral(":/schema/0004_send_at.sql")));
            setSchemaVersion(db, 4);
        });
    }
    if (v < 5) {
        runStep(db, [&] {
            execAll(db, readResource(QStringLiteral(":/schema/0005_snooze.sql")));
            setSchemaVersion(db, 5);
        });
    }
}

}  // namespace fc::cache
