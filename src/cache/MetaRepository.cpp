#include "MetaRepository.h"

#include "Migrations.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace fc::cache {

QString MetaRepository::get(const QString& key) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT value FROM meta WHERE key = :k"));
    q.bindValue(QStringLiteral(":k"), key);
    if (q.exec() && q.next()) return q.value(0).toString();
    return {};
}

void MetaRepository::set(const QString& key, const QString& value) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO meta(key, value) VALUES(:k, :v) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.bindValue(QStringLiteral(":k"), key);
    q.bindValue(QStringLiteral(":v"), value);
    q.exec();
}

QString MetaRepository::historyId() { return get(QStringLiteral("history_id")); }
void    MetaRepository::setHistoryId(const QString& v) {
    set(QStringLiteral("history_id"), v);
}

}  // namespace fc::cache
