#include "OutboxRepository.h"

#include "Migrations.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace fc::cache {

qint64 OutboxRepository::enqueue(const OutboxItem& item) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO outbox(state, rfc5322_blob, thread_id, "
        "                    in_reply_to_message_id, created_at, attempt_count, "
        "                    next_retry_at, send_at) "
        "VALUES('queued', :blob, :tid, :rep, :created, 0, 0, :sendAt)"));
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

std::vector<OutboxItem> OutboxRepository::dueForSend() {
    auto db = databaseHandle();
    std::vector<OutboxItem> out;
    QSqlQuery q(db);
    // Two due conditions: (1) the next_retry_at backoff window has
    // elapsed AND (2) the user-scheduled send_at (if any) has reached
    // wall-clock. send_at IS NULL means "send immediately, just respect
    // the retry backoff" — that's the path 99% of messages take.
    q.prepare(QStringLiteral(
        "SELECT id, state, rfc5322_blob, thread_id, in_reply_to_message_id, "
        "       created_at, attempt_count, next_retry_at, send_at, last_error "
        "FROM outbox "
        "WHERE state IN ('queued', 'failed') "
        "  AND next_retry_at <= :now "
        "  AND (send_at IS NULL OR send_at <= :now) "
        "ORDER BY id"));
    q.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) return out;
    while (q.next()) {
        OutboxItem i;
        i.id                  = q.value(0).toLongLong();
        i.state               = q.value(1).toString();
        i.rfc5322             = q.value(2).toByteArray();
        i.threadId            = q.value(3).toString();
        i.inReplyToMessageId  = q.value(4).toString();
        i.createdAt           = q.value(5).toLongLong();
        i.attemptCount        = q.value(6).toInt();
        i.nextRetryAt         = q.value(7).toLongLong();
        i.sendAt              = q.value(8).toLongLong();
        i.lastError           = q.value(9).toString();
        out.push_back(std::move(i));
    }
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

void OutboxRepository::markFailed(qint64 id, const QString& err, qint64 nextRetryAt) {
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

}  // namespace fc::cache
