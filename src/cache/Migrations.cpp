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
}

}  // namespace fc::cache
