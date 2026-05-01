#include "MessageRepository.h"

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

QString joinJsonArray(const QStringList& xs) {
    QJsonArray a;
    for (const auto& x : xs) a.append(x);
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

QStringList splitJsonArray(const QString& s) {
    QStringList out;
    for (const auto& v : QJsonDocument::fromJson(s.toUtf8()).array()) {
        out << v.toString();
    }
    return out;
}

fc::Message rowToMessage(const QSqlQuery& q) {
    fc::Message m;
    m.id              = q.value(QStringLiteral("id")).toString();
    m.threadId        = q.value(QStringLiteral("thread_id")).toString();
    m.historyId       = q.value(QStringLiteral("history_id")).toString();
    m.internalDate    = q.value(QStringLiteral("internal_date")).toLongLong();
    m.sizeEstimate    = q.value(QStringLiteral("size_estimate")).toInt();
    m.fromAddr        = q.value(QStringLiteral("from_addr")).toString();
    m.fromName        = q.value(QStringLiteral("from_name")).toString();
    m.toAddrs         = splitJsonArray(q.value(QStringLiteral("to_addrs")).toString());
    m.ccAddrs         = splitJsonArray(q.value(QStringLiteral("cc_addrs")).toString());
    m.bccAddrs        = splitJsonArray(q.value(QStringLiteral("bcc_addrs")).toString());
    m.replyTo         = q.value(QStringLiteral("reply_to")).toString();
    m.subject         = q.value(QStringLiteral("subject")).toString();
    m.snippet         = q.value(QStringLiteral("snippet")).toString();
    m.bodyText        = q.value(QStringLiteral("body_text")).toString();
    m.bodyHtmlPresent = q.value(QStringLiteral("body_html_present")).toBool();
    m.isUnread        = q.value(QStringLiteral("is_unread")).toBool();
    m.isStarred       = q.value(QStringLiteral("is_starred")).toBool();
    m.isImportant     = q.value(QStringLiteral("is_important")).toBool();
    m.hasAttachment   = q.value(QStringLiteral("has_attachment")).toBool();
    return m;
}

void writeLabelEdges(QSqlDatabase& db, const QString& messageId,
                     const QStringList& labelIds) {
    QSqlQuery del(db);
    del.prepare(QStringLiteral(
        "DELETE FROM message_labels WHERE message_id = :m"));
    del.bindValue(QStringLiteral(":m"), messageId);
    del.exec();

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO message_labels(message_id, label_id) "
        "VALUES (:m, :l)"));
    for (const auto& l : labelIds) {
        ins.bindValue(QStringLiteral(":m"), messageId);
        ins.bindValue(QStringLiteral(":l"), l);
        ins.exec();
    }
}

}  // namespace

qint64 MessageRepository::upsert(const fc::Message& m) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO messages(id, thread_id, history_id, internal_date, "
        "  size_estimate, from_addr, from_name, to_addrs, cc_addrs, bcc_addrs, "
        "  reply_to, subject, snippet, is_unread, is_starred, is_important, "
        "  has_attachment, body_text, body_html_present, fetched_format, "
        "  bytes_cached, last_accessed_at, created_at) "
        "VALUES(:id, :thread_id, :history_id, :internal_date, :size_estimate, "
        "  :from_addr, :from_name, :to_addrs, :cc_addrs, :bcc_addrs, :reply_to, "
        "  :subject, :snippet, :is_unread, :is_starred, :is_important, "
        "  :has_attachment, :body_text, :body_html_present, :fetched_format, "
        "  :bytes_cached, :last_accessed_at, :created_at) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  history_id     = excluded.history_id, "
        "  internal_date  = excluded.internal_date, "
        "  from_addr      = excluded.from_addr, "
        "  from_name      = excluded.from_name, "
        "  to_addrs       = excluded.to_addrs, "
        "  cc_addrs       = excluded.cc_addrs, "
        "  bcc_addrs      = excluded.bcc_addrs, "
        "  reply_to       = excluded.reply_to, "
        "  subject        = excluded.subject, "
        "  snippet        = excluded.snippet, "
        "  is_unread      = excluded.is_unread, "
        "  is_starred     = excluded.is_starred, "
        "  is_important   = excluded.is_important, "
        "  has_attachment = excluded.has_attachment, "
        "  body_text      = COALESCE(NULLIF(excluded.body_text, ''), messages.body_text), "
        "  body_html_present = excluded.body_html_present, "
        "  fetched_format = excluded.fetched_format"));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    q.bindValue(QStringLiteral(":id"),                m.id);
    q.bindValue(QStringLiteral(":thread_id"),         m.threadId);
    q.bindValue(QStringLiteral(":history_id"),        m.historyId);
    q.bindValue(QStringLiteral(":internal_date"),     m.internalDate);
    q.bindValue(QStringLiteral(":size_estimate"),     m.sizeEstimate);
    q.bindValue(QStringLiteral(":from_addr"),         m.fromAddr);
    q.bindValue(QStringLiteral(":from_name"),         m.fromName);
    q.bindValue(QStringLiteral(":to_addrs"),          joinJsonArray(m.toAddrs));
    q.bindValue(QStringLiteral(":cc_addrs"),          joinJsonArray(m.ccAddrs));
    q.bindValue(QStringLiteral(":bcc_addrs"),         joinJsonArray(m.bccAddrs));
    q.bindValue(QStringLiteral(":reply_to"),          m.replyTo);
    q.bindValue(QStringLiteral(":subject"),           m.subject);
    q.bindValue(QStringLiteral(":snippet"),           m.snippet);
    q.bindValue(QStringLiteral(":is_unread"),         m.isUnread ? 1 : 0);
    q.bindValue(QStringLiteral(":is_starred"),        m.isStarred ? 1 : 0);
    q.bindValue(QStringLiteral(":is_important"),      m.isImportant ? 1 : 0);
    q.bindValue(QStringLiteral(":has_attachment"),    m.hasAttachment ? 1 : 0);
    q.bindValue(QStringLiteral(":body_text"),         m.bodyText);
    q.bindValue(QStringLiteral(":body_html_present"), m.bodyHtmlPresent ? 1 : 0);
    q.bindValue(QStringLiteral(":fetched_format"),
                m.bodyText.isEmpty() ? QStringLiteral("metadata")
                                     : QStringLiteral("full"));
    q.bindValue(QStringLiteral(":bytes_cached"), m.bodyText.size());
    q.bindValue(QStringLiteral(":last_accessed_at"), now);
    q.bindValue(QStringLiteral(":created_at"),       now);

    if (!q.exec()) {
        qWarning("MessageRepository::upsert: %s",
                 qUtf8Printable(q.lastError().text()));
        return 0;
    }

    QSqlQuery threadUp(db);
    threadUp.prepare(QStringLiteral(
        "INSERT INTO threads(id, history_id, snippet, last_message_internal_date) "
        "VALUES(:id, :h, :s, :d) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  history_id = excluded.history_id, "
        "  snippet    = excluded.snippet, "
        "  last_message_internal_date = MAX(threads.last_message_internal_date, excluded.last_message_internal_date)"));
    threadUp.bindValue(QStringLiteral(":id"), m.threadId);
    threadUp.bindValue(QStringLiteral(":h"),  m.historyId);
    threadUp.bindValue(QStringLiteral(":s"),  m.snippet);
    threadUp.bindValue(QStringLiteral(":d"),  m.internalDate);
    threadUp.exec();

    writeLabelEdges(db, m.id, m.labelIds);
    return q.lastInsertId().toLongLong();
}

std::vector<fc::Message> MessageRepository::listByLabel(const QString& labelId,
                                                        int limit, int offset) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT m.* FROM messages m "
        "JOIN message_labels ml ON ml.message_id = m.id "
        "WHERE ml.label_id = :l "
        "ORDER BY m.internal_date DESC "
        "LIMIT :lim OFFSET :off"));
    q.bindValue(QStringLiteral(":l"),   labelId);
    q.bindValue(QStringLiteral(":lim"), limit);
    q.bindValue(QStringLiteral(":off"), offset);
    if (!q.exec()) return out;
    while (q.next()) out.push_back(rowToMessage(q));
    return out;
}

namespace {

// Splits a user-typed query into whitespace-separated terms, double-quotes
// each one (escaping embedded quotes), and joins them with " AND ". This
// gives FTS5 a syntactically-valid expression regardless of the operators,
// punctuation, or unbalanced quotes the user typed.
QString normaliseFtsQuery(const QString& raw) {
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) return {};
    QStringList terms = trimmed.split(QRegularExpression(QStringLiteral("\\s+")),
                                      Qt::SkipEmptyParts);
    QStringList quoted;
    quoted.reserve(terms.size());
    for (QString t : terms) {
        // Drop tokens that contain no FTS-meaningful characters.
        t.remove(QChar('"'));
        if (t.isEmpty()) continue;
        quoted << QStringLiteral("\"%1\"").arg(t);
    }
    if (quoted.isEmpty()) return {};
    return quoted.join(QStringLiteral(" AND "));
}

}  // namespace

std::vector<fc::Message> MessageRepository::searchFts(const QString& query, int limit) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;
    const QString fts = normaliseFtsQuery(query);
    if (fts.isEmpty()) return out;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT m.* FROM messages m "
        "JOIN messages_fts f ON f.rowid = m.rowid "
        "WHERE messages_fts MATCH :q "
        "ORDER BY rank, m.internal_date DESC "
        "LIMIT :lim"));
    q.bindValue(QStringLiteral(":q"),   fts);
    q.bindValue(QStringLiteral(":lim"), limit);
    if (!q.exec()) {
        qWarning("FTS search failed: %s", qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) out.push_back(rowToMessage(q));
    return out;
}

fc::Message MessageRepository::byId(const QString& id) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT * FROM messages WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), id);
    if (q.exec() && q.next()) return rowToMessage(q);
    return {};
}

bool MessageRepository::exists(const QString& id) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT 1 FROM messages WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), id);
    return q.exec() && q.next();
}

void MessageRepository::applyLabelDiff(const QString& messageId,
                                       const QStringList& added,
                                       const QStringList& removed) {
    auto db = databaseHandle();

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO message_labels(message_id, label_id) "
        "VALUES(:m, :l)"));
    for (const auto& l : added) {
        ins.bindValue(QStringLiteral(":m"), messageId);
        ins.bindValue(QStringLiteral(":l"), l);
        ins.exec();
    }

    QSqlQuery del(db);
    del.prepare(QStringLiteral(
        "DELETE FROM message_labels WHERE message_id = :m AND label_id = :l"));
    for (const auto& l : removed) {
        del.bindValue(QStringLiteral(":m"), messageId);
        del.bindValue(QStringLiteral(":l"), l);
        del.exec();
    }

    QSqlQuery flags(db);
    flags.prepare(QStringLiteral(
        "UPDATE messages SET "
        "  is_unread  = (SELECT COUNT(*) FROM message_labels WHERE message_id = :m AND label_id = 'UNREAD')  > 0, "
        "  is_starred = (SELECT COUNT(*) FROM message_labels WHERE message_id = :m AND label_id = 'STARRED') > 0 "
        "WHERE id = :m"));
    flags.bindValue(QStringLiteral(":m"), messageId);
    flags.exec();
}

void MessageRepository::markAccessed(const QString& id) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE messages SET last_accessed_at = :t WHERE id = :id"));
    q.bindValue(QStringLiteral(":t"),  QDateTime::currentMSecsSinceEpoch());
    q.bindValue(QStringLiteral(":id"), id);
    q.exec();
}

}  // namespace fc::cache
