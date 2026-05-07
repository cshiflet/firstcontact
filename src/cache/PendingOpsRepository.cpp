#include "PendingOpsRepository.h"

#include "Database.h"
#include "Migrations.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace fc::cache {

namespace {

QString encodePayload(const QStringList& add, const QStringList& remove) {
    QJsonArray a; for (const auto& s : add)    a.append(s);
    QJsonArray r; for (const auto& s : remove) r.append(s);
    QJsonObject o{
        {QStringLiteral("addLabelIds"),    a},
        {QStringLiteral("removeLabelIds"), r},
    };
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void decodePayload(const QString& s, QStringList& add, QStringList& remove) {
    const auto o = QJsonDocument::fromJson(s.toUtf8()).object();
    for (const auto v : o.value(QStringLiteral("addLabelIds")).toArray())    add    << v.toString();
    for (const auto v : o.value(QStringLiteral("removeLabelIds")).toArray()) remove << v.toString();
}

PendingOp rowFromQuery(const QSqlQuery& q) {
    PendingOp p;
    p.id        = q.value(0).toLongLong();
    p.accountId = q.value(1).toString();
    p.opType    = q.value(2).toString();
    p.messageId = q.value(3).toString();
    decodePayload(q.value(4).toString(), p.addLabels, p.removeLabels);
    p.createdAt = q.value(5).toLongLong();
    p.attempts  = q.value(6).toInt();
    p.lastError = q.value(7).toString();
    return p;
}

const char* kSelectColumns =
    "id, account_id, op_type, message_id, payload, created_at, attempts, last_error";

}  // namespace

qint64 PendingOpsRepository::enqueueModify(const QString& accountId,
                                            const QString& messageId,
                                            const QStringList& addLabels,
                                            const QStringList& removeLabels) {
    if (accountId.isEmpty()) {
        qWarning("PendingOpsRepository::enqueueModify called without an "
                 "accountId; dropped");
        return 0;
    }
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO pending_ops(account_id, op_type, message_id, payload, "
        "                         created_at, attempts) "
        "VALUES(:a, 'modify', :m, :p, :t, 0)"));
    q.bindValue(QStringLiteral(":a"), accountId);
    q.bindValue(QStringLiteral(":m"), messageId);
    q.bindValue(QStringLiteral(":p"), encodePayload(addLabels, removeLabels));
    q.bindValue(QStringLiteral(":t"), QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        qWarning("PendingOpsRepository::enqueueModify: %s",
                 qUtf8Printable(q.lastError().text()));
        return 0;
    }
    return q.lastInsertId().toLongLong();
}

std::vector<PendingOp> PendingOpsRepository::due(const QString& accountId) {
    auto db = databaseHandle();
    std::vector<PendingOp> out;
    if (accountId.isEmpty()) return out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT %1 FROM pending_ops WHERE account_id = :a ORDER BY id")
                  .arg(QString::fromLatin1(kSelectColumns)));
    q.bindValue(QStringLiteral(":a"), accountId);
    if (!q.exec()) return out;
    while (q.next()) out.push_back(rowFromQuery(q));
    return out;
}

std::vector<PendingOp> PendingOpsRepository::dueAllAccounts() {
    auto db = databaseHandle();
    std::vector<PendingOp> out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT %1 FROM pending_ops ORDER BY account_id, id")
                .arg(QString::fromLatin1(kSelectColumns)))) return out;
    while (q.next()) out.push_back(rowFromQuery(q));
    return out;
}

void PendingOpsRepository::markAttempt(qint64 id, const QString& err) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE pending_ops SET attempts = attempts + 1, last_error = :err "
        "WHERE id = :id"));
    q.bindValue(QStringLiteral(":err"), err);
    q.bindValue(QStringLiteral(":id"),  id);
    q.exec();
}

void PendingOpsRepository::remove(qint64 id) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM pending_ops WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), id);
    q.exec();
}

// ---------- legacy zero-arg overloads ----------

qint64 PendingOpsRepository::enqueueModify(const QString& messageId,
                                            const QStringList& addLabels,
                                            const QStringList& removeLabels) {
    return enqueueModify(Database::defaultAccountId(), messageId,
                         addLabels, removeLabels);
}

std::vector<PendingOp> PendingOpsRepository::due() {
    return due(Database::defaultAccountId());
}

}  // namespace fc::cache
