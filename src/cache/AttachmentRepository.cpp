#include "AttachmentRepository.h"

#include "Database.h"
#include "Migrations.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace fc::cache {

void AttachmentRepository::replaceForMessage(
        const QString& accountId,
        const QString& messageId,
        const std::vector<fc::Attachment>& attachments) {
    if (accountId.isEmpty()) return;
    auto db = databaseHandle();

    QSqlQuery del(db);
    del.prepare(QStringLiteral(
        "DELETE FROM attachments WHERE account_id = :a AND message_id = :m"));
    del.bindValue(QStringLiteral(":a"), accountId);
    del.bindValue(QStringLiteral(":m"), messageId);
    if (!del.exec()) {
        qWarning("AttachmentRepository::replaceForMessage delete: %s",
                 qUtf8Printable(del.lastError().text()));
        return;
    }

    if (attachments.empty()) return;

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO attachments(account_id, id, message_id, filename, "
        "                         mime_type, size, local_path) "
        "VALUES(:a, :id, :m, :fn, :mt, :sz, :lp)"));
    for (const auto& a : attachments) {
        const QString rowId = a.id.isEmpty()
            ? messageId + QStringLiteral(":") + a.filename
            : a.id;
        ins.bindValue(QStringLiteral(":a"),  accountId);
        ins.bindValue(QStringLiteral(":id"), rowId);
        ins.bindValue(QStringLiteral(":m"),  messageId);
        ins.bindValue(QStringLiteral(":fn"), a.filename);
        ins.bindValue(QStringLiteral(":mt"), a.mimeType);
        ins.bindValue(QStringLiteral(":sz"), a.size);
        ins.bindValue(QStringLiteral(":lp"), a.localPath);
        if (!ins.exec()) {
            qWarning("AttachmentRepository::replaceForMessage insert "
                     "(acc=%s, msg=%s, file=%s): %s",
                     qUtf8Printable(accountId),
                     qUtf8Printable(messageId),
                     qUtf8Printable(a.filename),
                     qUtf8Printable(ins.lastError().text()));
        }
    }
}

std::vector<fc::Attachment> AttachmentRepository::byMessage(
        const QString& accountId, const QString& messageId) {
    auto db = databaseHandle();
    std::vector<fc::Attachment> out;
    if (accountId.isEmpty()) return out;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, filename, mime_type, size, local_path "
        "FROM attachments WHERE account_id = :a AND message_id = :m "
        "ORDER BY filename"));
    q.bindValue(QStringLiteral(":a"), accountId);
    q.bindValue(QStringLiteral(":m"), messageId);
    if (!q.exec()) {
        qWarning("AttachmentRepository::byMessage: %s",
                 qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) {
        fc::Attachment a;
        a.id        = q.value(0).toString();
        a.filename  = q.value(1).toString();
        a.mimeType  = q.value(2).toString();
        a.size      = q.value(3).toInt();
        a.localPath = q.value(4).toString();
        out.push_back(std::move(a));
    }
    return out;
}

void AttachmentRepository::markDownloaded(const QString& accountId,
                                           const QString& attachmentId,
                                           const QString& localPath) {
    if (accountId.isEmpty()) return;
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE attachments SET local_path = :lp "
        "WHERE account_id = :a AND id = :id"));
    q.bindValue(QStringLiteral(":lp"), localPath);
    q.bindValue(QStringLiteral(":a"),  accountId);
    q.bindValue(QStringLiteral(":id"), attachmentId);
    q.exec();
}

// ---------- legacy zero-arg overloads ----------

void AttachmentRepository::replaceForMessage(
        const QString& messageId,
        const std::vector<fc::Attachment>& attachments) {
    replaceForMessage(Database::defaultAccountId(), messageId, attachments);
}

std::vector<fc::Attachment> AttachmentRepository::byMessage(
        const QString& messageId) {
    return byMessage(Database::defaultAccountId(), messageId);
}

void AttachmentRepository::markDownloaded(const QString& attachmentId,
                                           const QString& localPath) {
    markDownloaded(Database::defaultAccountId(), attachmentId, localPath);
}

}  // namespace fc::cache
