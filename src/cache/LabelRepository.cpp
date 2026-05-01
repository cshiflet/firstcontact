#include "LabelRepository.h"

#include "Migrations.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace fc::cache {

void LabelRepository::upsert(const LabelRow& l) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO labels(id, name, type, parent_id, color_bg, color_fg, "
        "                   unread_count, total_count) "
        "VALUES(:id, :name, :type, :parent, :bg, :fg, :u, :t) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  name      = excluded.name, "
        "  parent_id = excluded.parent_id, "
        "  color_bg  = excluded.color_bg, "
        "  color_fg  = excluded.color_fg, "
        "  unread_count = excluded.unread_count, "
        "  total_count  = excluded.total_count"));
    q.bindValue(QStringLiteral(":id"),     l.id);
    q.bindValue(QStringLiteral(":name"),   l.name);
    q.bindValue(QStringLiteral(":type"),   l.type);
    // parent_id is purely informational — Gmail's API doesn't expose a parent
    // id and the visual tree comes from "/"-splitting the label name in
    // LabelTreeModel. We persist NULL unless callers explicitly hand us one.
    if (l.parentId.isEmpty()) {
        q.bindValue(QStringLiteral(":parent"), QVariant(QMetaType(QMetaType::QString)));
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

std::vector<LabelRow> LabelRepository::all() {
    auto db = databaseHandle();
    std::vector<LabelRow> out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT id, name, type, parent_id, color_bg, color_fg, "
            "       unread_count, total_count "
            "FROM labels ORDER BY name"))) return out;
    while (q.next()) {
        LabelRow l;
        l.id          = q.value(0).toString();
        l.name        = q.value(1).toString();
        l.type        = q.value(2).toString();
        l.parentId    = q.value(3).toString();
        l.colorBg     = q.value(4).toString();
        l.colorFg     = q.value(5).toString();
        l.unreadCount = q.value(6).toInt();
        l.totalCount  = q.value(7).toInt();
        out.push_back(std::move(l));
    }
    return out;
}

LabelRow LabelRepository::byId(const QString& id) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, name, type, parent_id, color_bg, color_fg, "
        "       unread_count, total_count FROM labels WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec() || !q.next()) return {};
    LabelRow l;
    l.id          = q.value(0).toString();
    l.name        = q.value(1).toString();
    l.type        = q.value(2).toString();
    l.parentId    = q.value(3).toString();
    l.colorBg     = q.value(4).toString();
    l.colorFg     = q.value(5).toString();
    l.unreadCount = q.value(6).toInt();
    l.totalCount  = q.value(7).toInt();
    return l;
}

void LabelRepository::remove(const QString& id) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM labels WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), id);
    q.exec();
}

void LabelRepository::recomputeCounts() {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.exec(QStringLiteral(
        "UPDATE labels SET "
        "  total_count  = (SELECT COUNT(*) FROM message_labels WHERE label_id = labels.id), "
        "  unread_count = (SELECT COUNT(*) FROM message_labels ml "
        "                  JOIN messages m ON m.id = ml.message_id "
        "                  WHERE ml.label_id = labels.id AND m.is_unread = 1)"));
}

}  // namespace fc::cache
