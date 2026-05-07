#include "LabelRepository.h"

#include "Database.h"
#include "Migrations.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace fc::cache {

namespace {

LabelRow rowFromQuery(const QString& accountId, const QSqlQuery& q) {
    LabelRow l;
    l.accountId   = accountId;
    l.id          = q.value(QStringLiteral("id")).toString();
    l.name        = q.value(QStringLiteral("name")).toString();
    l.type        = q.value(QStringLiteral("type")).toString();
    l.parentId    = q.value(QStringLiteral("parent_id")).toString();
    l.colorBg     = q.value(QStringLiteral("color_bg")).toString();
    l.colorFg     = q.value(QStringLiteral("color_fg")).toString();
    l.unreadCount = q.value(QStringLiteral("unread_count")).toInt();
    l.totalCount  = q.value(QStringLiteral("total_count")).toInt();
    return l;
}

}  // namespace

void LabelRepository::upsert(const QString& accountId, const LabelRow& l) {
    if (accountId.isEmpty()) {
        qWarning("LabelRepository::upsert called without an accountId; "
                 "label %s dropped", qUtf8Printable(l.id));
        return;
    }
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO labels(account_id, id, name, type, parent_id, "
        "                   color_bg, color_fg, unread_count, total_count) "
        "VALUES(:a, :id, :name, :type, :parent, :bg, :fg, :u, :t) "
        "ON CONFLICT(account_id, id) DO UPDATE SET "
        "  name      = excluded.name, "
        "  parent_id = excluded.parent_id, "
        "  color_bg  = excluded.color_bg, "
        "  color_fg  = excluded.color_fg, "
        "  unread_count = excluded.unread_count, "
        "  total_count  = excluded.total_count"));
    q.bindValue(QStringLiteral(":a"),      accountId);
    q.bindValue(QStringLiteral(":id"),     l.id);
    q.bindValue(QStringLiteral(":name"),   l.name);
    q.bindValue(QStringLiteral(":type"),   l.type);
    if (l.parentId.isEmpty()) {
        q.bindValue(QStringLiteral(":parent"),
                    QVariant(QMetaType(QMetaType::QString)));
    } else {
        q.bindValue(QStringLiteral(":parent"), l.parentId);
    }
    q.bindValue(QStringLiteral(":bg"),     l.colorBg);
    q.bindValue(QStringLiteral(":fg"),     l.colorFg);
    q.bindValue(QStringLiteral(":u"),      l.unreadCount);
    q.bindValue(QStringLiteral(":t"),      l.totalCount);
    if (!q.exec()) qWarning("LabelRepository::upsert: %s",
                            qUtf8Printable(q.lastError().text()));
}

std::vector<LabelRow> LabelRepository::all(const QString& accountId) {
    auto db = databaseHandle();
    std::vector<LabelRow> out;
    if (accountId.isEmpty()) return out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, name, type, parent_id, color_bg, color_fg, "
        "       unread_count, total_count "
        "FROM labels WHERE account_id = :a ORDER BY name"));
    q.bindValue(QStringLiteral(":a"), accountId);
    if (!q.exec()) return out;
    while (q.next()) out.push_back(rowFromQuery(accountId, q));
    return out;
}

LabelRow LabelRepository::byId(const QString& accountId, const QString& id) {
    auto db = databaseHandle();
    if (accountId.isEmpty()) return {};
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, name, type, parent_id, color_bg, color_fg, "
        "       unread_count, total_count FROM labels "
        "WHERE account_id = :a AND id = :id"));
    q.bindValue(QStringLiteral(":a"),  accountId);
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec() || !q.next()) return {};
    return rowFromQuery(accountId, q);
}

void LabelRepository::remove(const QString& accountId, const QString& id) {
    if (accountId.isEmpty()) return;
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "DELETE FROM labels WHERE account_id = :a AND id = :id"));
    q.bindValue(QStringLiteral(":a"),  accountId);
    q.bindValue(QStringLiteral(":id"), id);
    q.exec();
}

void LabelRepository::recomputeCounts(const QString& accountId) {
    if (accountId.isEmpty()) return;
    auto db = databaseHandle();
    QSqlQuery q(db);
    // Account-scoped: count message_labels rows + unread messages WITHIN
    // this account only. Cross-account row mixing would otherwise inflate
    // the counts we render in the sidebar.
    q.prepare(QStringLiteral(
        "UPDATE labels SET "
        "  total_count  = (SELECT COUNT(*) FROM message_labels ml "
        "                  WHERE ml.label_id = labels.id "
        "                    AND ml.account_id = labels.account_id), "
        "  unread_count = (SELECT COUNT(*) FROM message_labels ml "
        "                  JOIN messages m ON m.id = ml.message_id "
        "                                 AND m.account_id = ml.account_id "
        "                  WHERE ml.label_id = labels.id "
        "                    AND ml.account_id = labels.account_id "
        "                    AND m.is_unread = 1) "
        "WHERE account_id = :a"));
    q.bindValue(QStringLiteral(":a"), accountId);
    q.exec();
}

// ---------- legacy zero-arg overloads ----------

void LabelRepository::upsert(const LabelRow& l) {
    const QString aid = l.accountId.isEmpty()
        ? Database::defaultAccountId()
        : l.accountId;
    upsert(aid, l);
}

std::vector<LabelRow> LabelRepository::all() {
    return all(Database::defaultAccountId());
}

LabelRow LabelRepository::byId(const QString& id) {
    return byId(Database::defaultAccountId(), id);
}

void LabelRepository::remove(const QString& id) {
    remove(Database::defaultAccountId(), id);
}

void LabelRepository::recomputeCounts() {
    recomputeCounts(Database::defaultAccountId());
}

}  // namespace fc::cache
