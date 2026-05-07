#include "OutboxRepository.h"

#include "Database.h"
#include "Migrations.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace fc::cache {

namespace {

OutboxItem rowFromQuery(const QSqlQuery& q) {
    OutboxItem i;
    i.id                  = q.value(0).toLongLong();
    i.accountId           = q.value(1).toString();
    i.state               = q.value(2).toString();
    i.rfc5322             = q.value(3).toByteArray();
    i.threadId            = q.value(4).toString();
    i.inReplyToMessageId  = q.value(5).toString();
    i.createdAt           = q.value(6).toLongLong();
    i.attemptCount        = q.value(7).toInt();
    i.nextRetryAt         = q.value(8).toLongLong();
    i.sendAt              = q.value(9).toLongLong();
    i.lastError           = q.value(10).toString();
    return i;
}

const char* kSelectColumns =
    "id, account_id, state, rfc5322_blob, thread_id, in_reply_to_message_id, "
    "created_at, attempt_count, next_retry_at, send_at, last_error";

}  // namespace

qint64 OutboxRepository::enqueue(const QString& accountId,
                                 const OutboxItem& item) {
    if (accountId.isEmpty()) {
        qWarning("OutboxRepository::enqueue called without an accountId; dropped");
        return 0;
    }
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO outbox(account_id, state, rfc5322_blob, thread_id, "
        "                    in_reply_to_message_id, created_at, attempt_count, "
        "                    next_retry_at, send_at) "
        "VALUES(:a, 'queued', :blob, :tid, :rep, :created, 0, 0, :sendAt)"));
    q.bindValue(QStringLiteral(":a"),       accountId);
    q.bindValue(QStringLiteral(":blob"),    item.rfc5322);
    q.bindValue(QStringLiteral(":tid"),     item.threadId);
    q.bindValue(QStringLiteral(":rep"),     item.inReplyToMessageId);
    q.bindValue(QStringLiteral(":created"), QDateTime::currentMSecsSinceEpoch());
    if (item.sendAt > 0) {
        q.bindValue(QStringLiteral(":sendAt"), item.sendAt);
    } else {
        q.bindValue(QStringLiteral(":sendAt"),
                    QVariant(QMetaType(QMetaType::LongLong)));
    }
    if (!q.exec()) {
        qWarning("OutboxRepository::enqueue: %s",
                 qUtf8Printable(q.lastError().text()));
        return 0;
    }
    return q.lastInsertId().toLongLong();
}

std::vector<OutboxItem> OutboxRepository::dueForSend(const QString& accountId) {
    auto db = databaseHandle();
    std::vector<OutboxItem> out;
    if (accountId.isEmpty()) return out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT %1 FROM outbox "
        "WHERE account_id = :a "
        "  AND state IN ('queued', 'failed') "
        "  AND next_retry_at <= :now "
        "  AND (send_at IS NULL OR send_at <= :now) "
        "ORDER BY id").arg(QString::fromLatin1(kSelectColumns)));
    q.bindValue(QStringLiteral(":a"),   accountId);
    q.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) return out;
    while (q.next()) out.push_back(rowFromQuery(q));
    return out;
}

std::vector<OutboxItem> OutboxRepository::dueForSendAllAccounts() {
    auto db = databaseHandle();
    std::vector<OutboxItem> out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT %1 FROM outbox "
        "WHERE state IN ('queued', 'failed') "
        "  AND next_retry_at <= :now "
        "  AND (send_at IS NULL OR send_at <= :now) "
        "ORDER BY account_id, id").arg(QString::fromLatin1(kSelectColumns)));
    q.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) return out;
    while (q.next()) out.push_back(rowFromQuery(q));
    return out;
}

void OutboxRepository::markSending(qint64 id) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE outbox SET state = 'sending', last_attempt_at = :t, "
        "                  attempt_count = attempt_count + 1 "
        "WHERE id = :id"));
    q.bindValue(QStringLiteral(":t"),  QDateTime::currentMSecsSinceEpoch());
    q.bindValue(QStringLiteral(":id"), id);
    q.exec();
}

void OutboxRepository::markSent(qint64 id) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM outbox WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), id);
    q.exec();
}

void OutboxRepository::markFailed(qint64 id, const QString& err,
                                   qint64 nextRetryAt) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE outbox SET state = 'failed', last_error = :err, "
        "                  next_retry_at = :next "
        "WHERE id = :id"));
    q.bindValue(QStringLiteral(":err"),  err);
    q.bindValue(QStringLiteral(":next"), nextRetryAt);
    q.bindValue(QStringLiteral(":id"),   id);
    q.exec();
}

// ---------- legacy zero-arg overloads ----------

qint64 OutboxRepository::enqueue(const OutboxItem& item) {
    const QString aid = item.accountId.isEmpty()
        ? Database::defaultAccountId()
        : item.accountId;
    return enqueue(aid, item);
}

std::vector<OutboxItem> OutboxRepository::dueForSend() {
    return dueForSend(Database::defaultAccountId());
}

}  // namespace fc::cache
