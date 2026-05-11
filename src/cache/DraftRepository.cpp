#include "DraftRepository.h"

#include "Database.h"
#include "Migrations.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace fc::cache {

namespace {

QString joinJsonArray(const QStringList& xs) {
    QJsonArray a; for (const auto& x : xs) a.append(x);
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

QStringList splitJsonArray(const QString& s) {
    QStringList out;
    for (const auto& v : QJsonDocument::fromJson(s.toUtf8()).array()) out << v.toString();
    return out;
}

DraftRow rowFromQuery(const QSqlQuery& q) {
    DraftRow d;
    d.accountId           = q.value(QStringLiteral("account_id")).toString();
    d.id                  = q.value(QStringLiteral("id")).toString();
    d.messageId           = q.value(QStringLiteral("message_id")).toString();
    d.threadId            = q.value(QStringLiteral("thread_id")).toString();
    d.inReplyToMessageId  = q.value(QStringLiteral("in_reply_to_msg_id")).toString();
    d.subject             = q.value(QStringLiteral("subject")).toString();
    d.toAddrs             = splitJsonArray(q.value(QStringLiteral("to_addrs")).toString());
    d.ccAddrs             = splitJsonArray(q.value(QStringLiteral("cc_addrs")).toString());
    d.bccAddrs            = splitJsonArray(q.value(QStringLiteral("bcc_addrs")).toString());
    d.bodyText            = q.value(QStringLiteral("body_text")).toString();
    d.updatedAt           = q.value(QStringLiteral("updated_at")).toLongLong();
    d.dirty               = q.value(QStringLiteral("dirty")).toBool();
    if (!d.id.startsWith(QStringLiteral("tmp-"))) d.gmailDraftId = d.id;
    return d;
}

}  // namespace

QString DraftRepository::upsert(const QString& accountId, const DraftRow& d) {
    if (accountId.isEmpty()) {
        qWarning("DraftRepository::upsert called without an accountId; dropped");
        return {};
    }
    auto db = databaseHandle();
    DraftRow row = d;
    row.accountId = accountId;
    if (row.id.isEmpty()) {
        row.id = QStringLiteral("tmp-") +
                 QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    row.updatedAt = QDateTime::currentMSecsSinceEpoch();

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO drafts(account_id, id, message_id, thread_id, "
        "                    in_reply_to_msg_id, subject, to_addrs, cc_addrs, "
        "                    bcc_addrs, body_text, updated_at, dirty) "
        "VALUES(:a, :id, :mid, :tid, :rep, :sub, :to, :cc, :bcc, :body, :u, :dirty) "
        "ON CONFLICT(account_id, id) DO UPDATE SET "
        "  message_id          = excluded.message_id, "
        "  thread_id           = excluded.thread_id, "
        "  in_reply_to_msg_id  = excluded.in_reply_to_msg_id, "
        "  subject             = excluded.subject, "
        "  to_addrs            = excluded.to_addrs, "
        "  cc_addrs            = excluded.cc_addrs, "
        "  bcc_addrs           = excluded.bcc_addrs, "
        "  body_text           = excluded.body_text, "
        "  updated_at          = excluded.updated_at, "
        "  dirty               = excluded.dirty"));
    q.bindValue(QStringLiteral(":a"),    accountId);
    q.bindValue(QStringLiteral(":id"),   row.id);
    q.bindValue(QStringLiteral(":mid"),  row.messageId);
    q.bindValue(QStringLiteral(":tid"),  row.threadId);
    q.bindValue(QStringLiteral(":rep"),  row.inReplyToMessageId);
    q.bindValue(QStringLiteral(":sub"),  row.subject);
    q.bindValue(QStringLiteral(":to"),   joinJsonArray(row.toAddrs));
    q.bindValue(QStringLiteral(":cc"),   joinJsonArray(row.ccAddrs));
    q.bindValue(QStringLiteral(":bcc"),  joinJsonArray(row.bccAddrs));
    q.bindValue(QStringLiteral(":body"), row.bodyText);
    q.bindValue(QStringLiteral(":u"),    row.updatedAt);
    q.bindValue(QStringLiteral(":dirty"),row.dirty ? 1 : 0);
    if (!q.exec()) {
        qWarning("DraftRepository::upsert: %s",
                 qUtf8Printable(q.lastError().text()));
    }
    return row.id;
}

std::vector<DraftRow> DraftRepository::listLocal(const QString& accountId) {
    auto db = databaseHandle();
    std::vector<DraftRow> out;
    if (accountId.isEmpty()) return out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT * FROM drafts WHERE account_id = :a "
        "ORDER BY updated_at DESC"));
    q.bindValue(QStringLiteral(":a"), accountId);
    if (!q.exec()) return out;
    while (q.next()) out.push_back(rowFromQuery(q));
    return out;
}

DraftRow DraftRepository::byId(const QString& accountId, const QString& id) {
    auto db = databaseHandle();
    if (accountId.isEmpty()) return {};
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT * FROM drafts WHERE account_id = :a AND id = :id"));
    q.bindValue(QStringLiteral(":a"),  accountId);
    q.bindValue(QStringLiteral(":id"), id);
    if (q.exec() && q.next()) return rowFromQuery(q);
    return {};
}

std::vector<DraftRow> DraftRepository::dirtyDrafts(const QString& accountId) {
    auto db = databaseHandle();
    std::vector<DraftRow> out;
    if (accountId.isEmpty()) return out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT * FROM drafts WHERE account_id = :a AND dirty = 1 "
        "ORDER BY updated_at"));
    q.bindValue(QStringLiteral(":a"), accountId);
    if (!q.exec()) return out;
    while (q.next()) out.push_back(rowFromQuery(q));
    return out;
}

std::vector<DraftRow> DraftRepository::dirtyDraftsAllAccounts() {
    auto db = databaseHandle();
    std::vector<DraftRow> out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT * FROM drafts WHERE dirty = 1 "
            "ORDER BY account_id, updated_at"))) return out;
    while (q.next()) out.push_back(rowFromQuery(q));
    return out;
}

void DraftRepository::markSynced(const QString& accountId,
                                  const QString& localId,
                                  const QString& gmailDraftId) {
    if (accountId.isEmpty()) return;
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE drafts SET id = :new, dirty = 0 "
        "WHERE account_id = :a AND id = :old"));
    q.bindValue(QStringLiteral(":new"), gmailDraftId);
    q.bindValue(QStringLiteral(":a"),   accountId);
    q.bindValue(QStringLiteral(":old"), localId);
    q.exec();
}

void DraftRepository::remove(const QString& accountId, const QString& id) {
    if (accountId.isEmpty()) return;
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "DELETE FROM drafts WHERE account_id = :a AND id = :id"));
    q.bindValue(QStringLiteral(":a"),  accountId);
    q.bindValue(QStringLiteral(":id"), id);
    q.exec();
}

}  // namespace fc::cache
