#include "MessageRepository.h"

#include "AttachmentRepository.h"
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
    m.bodyHtml        = q.value(QStringLiteral("body_html")).toString();
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

    // Sub-select guards against FK violations: only insert when the label
    // actually exists in our labels table. Gmail can return label_ids we
    // haven't synced yet (e.g. CHAT, labels created on another client
    // between our syncs) and we'd rather drop the edge than have a missing
    // label fail the whole message upsert.
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO message_labels(message_id, label_id) "
        "SELECT :m, :l WHERE EXISTS (SELECT 1 FROM labels WHERE id = :l)"));
    for (const auto& l : labelIds) {
        ins.bindValue(QStringLiteral(":m"), messageId);
        ins.bindValue(QStringLiteral(":l"), l);
        if (!ins.exec()) {
            qWarning("writeLabelEdges (msg=%s, label=%s): %s",
                     qUtf8Printable(messageId), qUtf8Printable(l),
                     qUtf8Printable(ins.lastError().text()));
        }
    }
}

}  // namespace

qint64 MessageRepository::upsert(const fc::Message& m) {
    auto db = databaseHandle();

    // Atomic 3-step write: thread row first (the message FK requires it),
    // then the message itself, then the label edges. Wrapped in a transaction
    // so a failure in any step leaves the cache untouched.
    db.transaction();

    // 1. Thread upsert. messages.thread_id REFERENCES threads(id), so this
    //    has to land before the message INSERT or PRAGMA foreign_keys = ON
    //    rejects the message with "FOREIGN KEY constraint failed".
    {
        QSqlQuery threadUp(db);
        threadUp.prepare(QStringLiteral(
            "INSERT INTO threads(id, history_id, snippet, last_message_internal_date) "
            "VALUES(:id, :h, :s, :d) "
            "ON CONFLICT(id) DO UPDATE SET "
            "  history_id = excluded.history_id, "
            "  snippet    = excluded.snippet, "
            "  last_message_internal_date = MAX(threads.last_message_internal_date, "
            "                                   excluded.last_message_internal_date)"));
        threadUp.bindValue(QStringLiteral(":id"), m.threadId);
        threadUp.bindValue(QStringLiteral(":h"),  m.historyId);
        threadUp.bindValue(QStringLiteral(":s"),  m.snippet);
        threadUp.bindValue(QStringLiteral(":d"),  m.internalDate);
        if (!threadUp.exec()) {
            qWarning("MessageRepository::upsert (thread): %s",
                     qUtf8Printable(threadUp.lastError().text()));
            db.rollback();
            return 0;
        }
    }

    // 2. Message upsert.
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO messages(id, thread_id, history_id, internal_date, "
        "  size_estimate, from_addr, from_name, to_addrs, cc_addrs, bcc_addrs, "
        "  reply_to, subject, snippet, is_unread, is_starred, is_important, "
        "  has_attachment, body_text, body_html, body_html_present, "
        "  fetched_format, bytes_cached, last_accessed_at, created_at) "
        "VALUES(:id, :thread_id, :history_id, :internal_date, :size_estimate, "
        "  :from_addr, :from_name, :to_addrs, :cc_addrs, :bcc_addrs, :reply_to, "
        "  :subject, :snippet, :is_unread, :is_starred, :is_important, "
        "  :has_attachment, :body_text, :body_html, :body_html_present, "
        "  :fetched_format, :bytes_cached, :last_accessed_at, :created_at) "
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
        "  body_html      = COALESCE(NULLIF(excluded.body_html, ''), messages.body_html), "
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
    q.bindValue(QStringLiteral(":body_html"),         m.bodyHtml);
    q.bindValue(QStringLiteral(":body_html_present"), m.bodyHtmlPresent ? 1 : 0);
    q.bindValue(QStringLiteral(":fetched_format"),
                m.bodyText.isEmpty() ? QStringLiteral("metadata")
                                     : QStringLiteral("full"));
    q.bindValue(QStringLiteral(":bytes_cached"),
                m.bodyText.size() + m.bodyHtml.size());
    q.bindValue(QStringLiteral(":last_accessed_at"), now);
    q.bindValue(QStringLiteral(":created_at"),       now);

    if (!q.exec()) {
        qWarning("MessageRepository::upsert (message): %s",
                 qUtf8Printable(q.lastError().text()));
        db.rollback();
        return 0;
    }

    // 3. Label edges. Skip any label_id that doesn't exist in the labels
    //    table — Gmail can return labels we haven't synced yet (e.g.
    //    CHAT, or labels created on another client between syncs) and we
    //    don't want a single missing label to fail the whole upsert.
    writeLabelEdges(db, m.id, m.labelIds);

    // 4. Attachments. The FK on attachments.message_id requires the message
    //    row to already exist (step 2). MessageParser populates m.attachments
    //    from the MIME parts of full-format fetches; metadata-only fetches
    //    leave it empty, in which case replaceForMessage clears any stale
    //    rows from a prior fetch.
    AttachmentRepository::replaceForMessage(m.id, m.attachments);

    db.commit();
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

// Collapses a per-message ranked CTE into one row per thread, hydrated
// with the LATEST message's fields (rn=1) plus whole-thread aggregates.
// Returns the SELECT statement built around an arbitrary CTE name and an
// inner WHERE clause that selects which messages enter the partition.
//
// Window functions need SQLite ≥ 3.25 (Ubuntu 24.04 ships 3.45+); fall
// back if we ever build against an older sqlite.
QString buildThreadRollupSql(const QString& innerWhere) {
    return QStringLiteral(
        "WITH ranked AS ("
        "  SELECT m.*, "
        "    ROW_NUMBER() OVER (PARTITION BY m.thread_id "
        "                        ORDER BY m.internal_date DESC) AS rn, "
        "    COUNT(*)   OVER (PARTITION BY m.thread_id) AS t_count, "
        "    SUM(m.is_unread)      OVER (PARTITION BY m.thread_id) AS t_unread, "
        "    MAX(m.is_starred)     OVER (PARTITION BY m.thread_id) AS t_starred, "
        "    MAX(m.has_attachment) OVER (PARTITION BY m.thread_id) AS t_attach "
        "  FROM messages m "
        "  %1"
        ") "
        "SELECT id, thread_id, history_id, internal_date, size_estimate, "
        "       from_addr, from_name, to_addrs, cc_addrs, bcc_addrs, reply_to, "
        "       subject, snippet, body_text, body_html, body_html_present, "
        "       is_unread, is_starred, is_important, has_attachment, "
        "       t_count, t_unread, t_starred, t_attach "
        "FROM ranked "
        "WHERE rn = 1 "
        "ORDER BY internal_date DESC "
        "LIMIT :lim OFFSET :off").arg(innerWhere);
}

void hydrateThreadAggregates(QSqlQuery& q, fc::Message& m) {
    m.threadCount          = q.value(QStringLiteral("t_count")).toInt();
    const int unread       = q.value(QStringLiteral("t_unread")).toInt();
    const int starred      = q.value(QStringLiteral("t_starred")).toInt();
    const int hasAttach    = q.value(QStringLiteral("t_attach")).toInt();
    m.threadHasUnread      = unread > 0;
    m.threadHasStarred     = starred > 0;
    m.threadHasAttachment  = hasAttach > 0;
    // For display, override the per-message flags with the thread-level
    // ones so the row-level icons in the message list reflect the whole
    // conversation rather than just the latest message.
    m.isUnread       = m.threadHasUnread;
    m.isStarred      = m.threadHasStarred;
    m.hasAttachment  = m.threadHasAttachment;
}

}  // namespace

std::vector<fc::Message> MessageRepository::listThreadsByLabel(
        const QString& labelId, int limit, int offset) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;

    QSqlQuery q(db);
    q.prepare(buildThreadRollupSql(QStringLiteral(
        "JOIN message_labels ml ON ml.message_id = m.id "
        "WHERE ml.label_id = :l")));
    q.bindValue(QStringLiteral(":l"),   labelId);
    q.bindValue(QStringLiteral(":lim"), limit);
    q.bindValue(QStringLiteral(":off"), offset);
    if (!q.exec()) {
        qWarning("listThreadsByLabel: %s",
                 qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) {
        auto m = rowToMessage(q);
        hydrateThreadAggregates(q, m);
        out.push_back(std::move(m));
    }
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

std::vector<fc::Message> MessageRepository::searchFtsThreads(
        const QString& query, int limit) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;
    const QString fts = normaliseFtsQuery(query);
    if (fts.isEmpty()) return out;

    // Collapse FTS hits into per-thread rows the same way listThreadsByLabel
    // does for label browsing. The CTE filters down to messages that match
    // the FTS query; window functions then pick the latest per thread and
    // fold in the aggregates.
    QSqlQuery q(db);
    q.prepare(buildThreadRollupSql(QStringLiteral(
        "JOIN messages_fts f ON f.rowid = m.rowid "
        "WHERE messages_fts MATCH :q")));
    q.bindValue(QStringLiteral(":q"),   fts);
    q.bindValue(QStringLiteral(":lim"), limit);
    q.bindValue(QStringLiteral(":off"), 0);
    if (!q.exec()) {
        qWarning("FTS thread search failed: %s",
                 qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) {
        auto m = rowToMessage(q);
        hydrateThreadAggregates(q, m);
        out.push_back(std::move(m));
    }
    return out;
}

fc::Message MessageRepository::byId(const QString& id) {
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT * FROM messages WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), id);
    if (q.exec() && q.next()) {
        auto m = rowToMessage(q);
        m.attachments = AttachmentRepository::byMessage(m.id);
        return m;
    }
    return {};
}

std::vector<fc::Message> MessageRepository::byThread(const QString& threadId) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;
    if (threadId.isEmpty()) return out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT * FROM messages WHERE thread_id = :t "
        "ORDER BY internal_date ASC"));
    q.bindValue(QStringLiteral(":t"), threadId);
    if (!q.exec()) return out;
    while (q.next()) out.push_back(rowToMessage(q));
    // Attachments live in their own table; pull them in a second pass rather
    // than trying to JOIN+aggregate. ReaderPane displays them per card.
    for (auto& m : out) {
        m.attachments = AttachmentRepository::byMessage(m.id);
    }
    return out;
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
