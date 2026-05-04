#include "AttachmentRepository.h"

#include "Migrations.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace fc::cache {

void AttachmentRepository::replaceForMessage(
        const QString& messageId,
        const std::vector<fc::Attachment>& attachments) {
    auto db = databaseHandle();

    QSqlQuery del(db);
    del.prepare(QStringLiteral(
        "DELETE FROM attachments WHERE message_id = :m"));
    del.bindValue(QStringLiteral(":m"), messageId);
    if (!del.exec()) {
        qWarning("AttachmentRepository::replaceForMessage delete: %s",
                 qUtf8Printable(del.lastError().text()));
        return;
    }

    if (attachments.empty()) return;

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO attachments(id, message_id, filename, mime_type, size, "
        "                         local_path) "
        "VALUES(:id, :m, :fn, :mt, :sz, :lp)"));
    for (const auto& a : attachments) {
        // Gmail occasionally returns the same attachmentId across multiple
        // parts of the same message (forwarded chains, signatures with the
        // same file); we synthesise a unique row id by suffixing the
        // filename so we don't collide on the PK.
        const QString rowId = a.id.isEmpty()
            ? messageId + QStringLiteral(":") + a.filename
            : a.id;
        ins.bindValue(QStringLiteral(":id"), rowId);
        ins.bindValue(QStringLiteral(":m"),  messageId);
        ins.bindValue(QStringLiteral(":fn"), a.filename);
        ins.bindValue(QStringLiteral(":mt"), a.mimeType);
        ins.bindValue(QStringLiteral(":sz"), a.size);
        ins.bindValue(QStringLiteral(":lp"), a.localPath);
        if (!ins.exec()) {
            qWarning("AttachmentRepository::replaceForMessage insert "
                     "(msg=%s, file=%s): %s",
                     qUtf8Printable(messageId),
                     qUtf8Printable(a.filename),
                     qUtf8Printable(ins.lastError().text()));
        }
    }
}

std::vector<fc::Attachment> AttachmentRepository::byMessage(
        const QString& messageId) {
    auto db = databaseHandle();
    std::vector<fc::Attachment> out;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, filename, mime_type, size, local_path "
        "FROM attachments WHERE message_id = :m "
        "ORDER BY filename"));
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

void AttachmentRepository::markDownloaded(const QString& attachmentId,
                                           const QString& localPath) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE attachments SET local_path = :lp WHERE id = :id"));
    q.bindValue(QStringLiteral(":lp"), localPath);
    q.bindValue(QStringLiteral(":id"), attachmentId);
    q.exec();
}

}  // namespace fc::cache
