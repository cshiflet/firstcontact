#include "MessageRepository.h"

#include "AttachmentRepository.h"
#include "Database.h"
#include "Migrations.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
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

fc::Message rowToMessage(const QString& accountId, const QSqlQuery& q) {
    fc::Message m;
    // Cross-account paths pass accountId="" and let the row's
    // account_id column drive; per-account paths pass the explicit
    // id (faster than re-reading the column when we already know).
    m.accountId       = accountId.isEmpty()
        ? q.value(QStringLiteral("account_id")).toString()
        : accountId;
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

void writeLabelEdges(QSqlDatabase& db, const QString& accountId,
                     const QString& messageId, const QStringList& labelIds) {
    QSqlQuery del(db);
    del.prepare(QStringLiteral(
        "DELETE FROM message_labels "
        "WHERE account_id = :a AND message_id = :m"));
    del.bindValue(QStringLiteral(":a"), accountId);
    del.bindValue(QStringLiteral(":m"), messageId);
    del.exec();

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO message_labels(account_id, message_id, label_id) "
        "SELECT :a, :m, :l "
        "WHERE EXISTS (SELECT 1 FROM labels "
        "              WHERE account_id = :a AND id = :l)"));
    for (const auto& l : labelIds) {
        ins.bindValue(QStringLiteral(":a"), accountId);
        ins.bindValue(QStringLiteral(":m"), messageId);
        ins.bindValue(QStringLiteral(":l"), l);
        if (!ins.exec()) {
            qWarning("writeLabelEdges (acc=%s, msg=%s, label=%s): %s",
                     qUtf8Printable(accountId),
                     qUtf8Printable(messageId), qUtf8Printable(l),
                     qUtf8Printable(ins.lastError().text()));
        }
    }
}

// Bulk-load message_labels rows for a batch of messages and stamp them
// onto each Message's labelIds field. Single SELECT per batch keeps the
// per-row delegate render path at one query, not N+1.
//
// Per-account variant: filters by a single account_id. Cross-account
// variant (accountId is empty) keys by (account_id, message_id) so two
// accounts' messages with coincidentally-equal ids don't bleed labels.
void hydrateLabelIds(const QString& accountId,
                     std::vector<fc::Message>& messages) {
    if (messages.empty()) return;
    auto db = fc::cache::databaseHandle();

    QString placeholders;
    placeholders.reserve(messages.size() * 2);
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i) placeholders += QLatin1Char(',');
        placeholders += QLatin1Char('?');
    }

    QSqlQuery q(db);
    if (accountId.isEmpty()) {
        // Cross-account: select (account_id, message_id, label_id) and
        // key the result by the composite. We'd ideally have a single
        // IN clause covering (account_id, id) tuples; sqlite supports
        // it via "WHERE (a, b) IN (VALUES (?, ?), …)" but binding
        // tuples to QSqlQuery is awkward. Simpler path: pull every
        // edge whose message_id is in the batch and post-filter by
        // accountId in C++.
        q.prepare(QStringLiteral(
            "SELECT account_id, message_id, label_id FROM message_labels "
            "WHERE message_id IN (%1)").arg(placeholders));
        for (const auto& m : messages) q.addBindValue(m.id);
        if (!q.exec()) {
            qWarning("hydrateLabelIds (all-accounts): %s",
                     qUtf8Printable(q.lastError().text()));
            return;
        }
        // Composite key: "<accountId>\t<messageId>" → labelIds.
        QHash<QString, QStringList> byMsg;
        while (q.next()) {
            const QString aid = q.value(0).toString();
            const QString mid = q.value(1).toString();
            byMsg[aid + QChar('\t') + mid] << q.value(2).toString();
        }
        for (auto& m : messages) {
            m.labelIds = byMsg.value(m.accountId + QChar('\t') + m.id);
        }
        return;
    }

    q.prepare(QStringLiteral(
        "SELECT message_id, label_id FROM message_labels "
        "WHERE account_id = ? AND message_id IN (%1)").arg(placeholders));
    q.addBindValue(accountId);
    for (const auto& m : messages) q.addBindValue(m.id);
    if (!q.exec()) {
        qWarning("hydrateLabelIds: %s",
                 qUtf8Printable(q.lastError().text()));
        return;
    }

    QHash<QString, QStringList> byMsg;
    while (q.next()) {
        byMsg[q.value(0).toString()] << q.value(1).toString();
    }
    for (auto& m : messages) m.labelIds = byMsg.value(m.id);
}

// Splits a user-typed query into whitespace-separated terms, double-quotes
// each one (escaping embedded quotes), and joins them with " AND ". Returns
// a syntactically-valid FTS5 expression for any user input.
QString normaliseFtsQuery(const QString& raw) {
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) return {};
    QStringList terms = trimmed.split(QRegularExpression(QStringLiteral("\\s+")),
                                      Qt::SkipEmptyParts);
    QStringList quoted;
    quoted.reserve(terms.size());
    for (QString t : terms) {
        t.remove(QChar('"'));
        if (t.isEmpty()) continue;
        quoted << QStringLiteral("\"%1\"").arg(t);
    }
    if (quoted.isEmpty()) return {};
    return quoted.join(QStringLiteral(" AND "));
}

// Collapses a per-message ranked CTE into one row per thread, hydrated
// with the LATEST message's fields (rn=1) plus whole-thread aggregates.
// PARTITION BY now includes account_id so a thread that exists in two
// accounts (rare but possible — same Gmail conversation across an alias
// + delegate) doesn't collapse across them.
QString buildThreadRollupSql(const QString& innerWhere) {
    return QStringLiteral(
        "WITH ranked AS ("
        "  SELECT m.*, "
        "    ROW_NUMBER() OVER (PARTITION BY m.account_id, m.thread_id "
        "                        ORDER BY m.internal_date DESC) AS rn, "
        "    COUNT(*)   OVER (PARTITION BY m.account_id, m.thread_id) AS t_count, "
        "    SUM(m.is_unread)      OVER (PARTITION BY m.account_id, m.thread_id) AS t_unread, "
        "    MAX(m.is_starred)     OVER (PARTITION BY m.account_id, m.thread_id) AS t_starred, "
        "    MAX(m.has_attachment) OVER (PARTITION BY m.account_id, m.thread_id) AS t_attach "
        "  FROM messages m "
        "  %1"
        ") "
        "SELECT account_id, id, thread_id, history_id, internal_date, "
        "       size_estimate, from_addr, from_name, to_addrs, cc_addrs, "
        "       bcc_addrs, reply_to, subject, snippet, body_text, body_html, "
        "       body_html_present, is_unread, is_starred, is_important, "
        "       has_attachment, t_count, t_unread, t_starred, t_attach "
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
    m.isUnread       = m.threadHasUnread;
    m.isStarred      = m.threadHasStarred;
    m.hasAttachment  = m.threadHasAttachment;
}

}  // namespace

qint64 MessageRepository::upsert(const QString& accountId, const fc::Message& m) {
    if (accountId.isEmpty()) {
        qWarning("MessageRepository::upsert called without an accountId; "
                 "msg %s dropped", qUtf8Printable(m.id));
        return 0;
    }
    auto db = databaseHandle();

    // Atomic 3-step write: thread row → message → label edges → attachments.
    // Wrapped in a transaction so a failure leaves the cache untouched.
    db.transaction();

    // 1. Thread upsert (composite PK on account_id + id).
    {
        QSqlQuery threadUp(db);
        threadUp.prepare(QStringLiteral(
            "INSERT INTO threads(account_id, id, history_id, snippet, "
            "                     last_message_internal_date) "
            "VALUES(:a, :id, :h, :s, :d) "
            "ON CONFLICT(account_id, id) DO UPDATE SET "
            "  history_id = excluded.history_id, "
            "  snippet    = excluded.snippet, "
            "  last_message_internal_date = MAX(threads.last_message_internal_date, "
            "                                   excluded.last_message_internal_date)"));
        threadUp.bindValue(QStringLiteral(":a"),  accountId);
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
        "INSERT INTO messages(account_id, id, thread_id, history_id, "
        "  internal_date, size_estimate, from_addr, from_name, to_addrs, "
        "  cc_addrs, bcc_addrs, reply_to, subject, snippet, is_unread, "
        "  is_starred, is_important, has_attachment, body_text, body_html, "
        "  body_html_present, fetched_format, bytes_cached, last_accessed_at, "
        "  created_at) "
        "VALUES(:a, :id, :thread_id, :history_id, :internal_date, "
        "  :size_estimate, :from_addr, :from_name, :to_addrs, :cc_addrs, "
        "  :bcc_addrs, :reply_to, :subject, :snippet, :is_unread, :is_starred, "
        "  :is_important, :has_attachment, :body_text, :body_html, "
        "  :body_html_present, :fetched_format, :bytes_cached, "
        "  :last_accessed_at, :created_at) "
        "ON CONFLICT(account_id, id) DO UPDATE SET "
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

    q.bindValue(QStringLiteral(":a"),                 accountId);
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

    // 3. Label edges (per-account scoped).
    writeLabelEdges(db, accountId, m.id, m.labelIds);

    // 4. Attachments.
    AttachmentRepository::replaceForMessage(accountId, m.id, m.attachments);

    db.commit();
    return q.lastInsertId().toLongLong();
}

std::vector<fc::Message> MessageRepository::listByLabel(
        const QString& accountId, const QString& labelId,
        int limit, int offset, bool unreadOnly) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;
    if (accountId.isEmpty()) return out;

    QSqlQuery q(db);
    // Two SQL variants — SQLite optimizes literal AND clauses much
    // better than dynamic flag expressions, and the unread filter
    // wants to make use of the is_unread column index.
    if (unreadOnly) {
        q.prepare(QStringLiteral(
            "SELECT m.* FROM messages m "
            "JOIN message_labels ml "
            "  ON ml.account_id = m.account_id AND ml.message_id = m.id "
            "WHERE m.account_id = :a AND ml.label_id = :l AND m.is_unread = 1 "
            "ORDER BY m.internal_date DESC "
            "LIMIT :lim OFFSET :off"));
    } else {
        q.prepare(QStringLiteral(
            "SELECT m.* FROM messages m "
            "JOIN message_labels ml "
            "  ON ml.account_id = m.account_id AND ml.message_id = m.id "
            "WHERE m.account_id = :a AND ml.label_id = :l "
            "ORDER BY m.internal_date DESC "
            "LIMIT :lim OFFSET :off"));
    }
    q.bindValue(QStringLiteral(":a"),   accountId);
    q.bindValue(QStringLiteral(":l"),   labelId);
    q.bindValue(QStringLiteral(":lim"), limit);
    q.bindValue(QStringLiteral(":off"), offset);
    if (!q.exec()) return out;
    while (q.next()) out.push_back(rowToMessage(accountId, q));
    hydrateLabelIds(accountId, out);
    return out;
}

std::vector<fc::Message> MessageRepository::listThreadsByLabel(
        const QString& accountId, const QString& labelId,
        int limit, int offset, bool unreadOnly) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;
    if (accountId.isEmpty()) return out;

    // Inner WHERE selects which messages (across this account) feed
    // the per-thread rollup. We wrap it in an extra SELECT for the
    // unread-only path so the t_unread aggregate (computed by the
    // rollup) can drive row visibility — pushing the unread filter
    // into the inner WHERE would skew t_count / t_starred / t_attach.
    QString sql = buildThreadRollupSql(QStringLiteral(
        "JOIN message_labels ml "
        "  ON ml.account_id = m.account_id AND ml.message_id = m.id "
        "WHERE m.account_id = :a AND ml.label_id = :l"));
    if (unreadOnly) {
        // Wrap; inner LIMIT becomes a large upper bound to cap
        // resource use on huge mailboxes, outer LIMIT/OFFSET is the
        // user-visible page. 10× headroom is plenty unless 90% of a
        // mailbox is read, in which case a follow-up query covers
        // the gap.
        sql = QStringLiteral(
            "SELECT * FROM (%1) WHERE t_unread > 0 "
            "ORDER BY internal_date DESC "
            "LIMIT :outerLim OFFSET :outerOff").arg(sql);
    }

    QSqlQuery q(db);
    q.prepare(sql);
    q.bindValue(QStringLiteral(":a"), accountId);
    q.bindValue(QStringLiteral(":l"), labelId);
    if (unreadOnly) {
        q.bindValue(QStringLiteral(":lim"),       qMax(limit * 10, 200));
        q.bindValue(QStringLiteral(":off"),       0);
        q.bindValue(QStringLiteral(":outerLim"),  limit);
        q.bindValue(QStringLiteral(":outerOff"),  offset);
    } else {
        q.bindValue(QStringLiteral(":lim"), limit);
        q.bindValue(QStringLiteral(":off"), offset);
    }
    if (!q.exec()) {
        qWarning("listThreadsByLabel: %s",
                 qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) {
        auto m = rowToMessage(accountId, q);
        hydrateThreadAggregates(q, m);
        out.push_back(std::move(m));
    }
    hydrateLabelIds(accountId, out);
    int multi = 0;
    for (const auto& m : out) if (m.threadCount > 1) ++multi;
    qInfo("listThreadsByLabel(acc=%s, label=%s, limit=%d, offset=%d): "
          "%zu threads (%d with >1 message)",
          qUtf8Printable(accountId), qUtf8Printable(labelId),
          limit, offset, out.size(), multi);
    return out;
}

std::vector<fc::Message> MessageRepository::searchFts(
        const QString& accountId, const QString& query, int limit) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;
    if (accountId.isEmpty()) return out;
    const QString fts = normaliseFtsQuery(query);
    if (fts.isEmpty()) return out;

    QSqlQuery q(db);
    // FTS5 exposes account_id via the UNINDEXED column added in 0006. We
    // could pre-filter inside MATCH, but the more obvious form (filter on
    // m.account_id after the join) reads cleanly and lets the planner use
    // the FTS rank for ordering.
    q.prepare(QStringLiteral(
        "SELECT m.* FROM messages m "
        "JOIN messages_fts f ON f.rowid = m.rowid "
        "WHERE messages_fts MATCH :q AND m.account_id = :a "
        "ORDER BY rank, m.internal_date DESC "
        "LIMIT :lim"));
    q.bindValue(QStringLiteral(":q"),   fts);
    q.bindValue(QStringLiteral(":a"),   accountId);
    q.bindValue(QStringLiteral(":lim"), limit);
    if (!q.exec()) {
        qWarning("FTS search failed: %s", qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) out.push_back(rowToMessage(accountId, q));
    hydrateLabelIds(accountId, out);
    return out;
}

std::vector<fc::Message> MessageRepository::searchFtsThreads(
        const QString& accountId, const QString& query, int limit) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;
    if (accountId.isEmpty()) return out;
    const QString fts = normaliseFtsQuery(query);
    if (fts.isEmpty()) return out;

    QSqlQuery q(db);
    q.prepare(buildThreadRollupSql(QStringLiteral(
        "JOIN messages_fts f ON f.rowid = m.rowid "
        "WHERE messages_fts MATCH :q AND m.account_id = :a")));
    q.bindValue(QStringLiteral(":q"),   fts);
    q.bindValue(QStringLiteral(":a"),   accountId);
    q.bindValue(QStringLiteral(":lim"), limit);
    q.bindValue(QStringLiteral(":off"), 0);
    if (!q.exec()) {
        qWarning("FTS thread search failed: %s",
                 qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) {
        auto m = rowToMessage(accountId, q);
        hydrateThreadAggregates(q, m);
        out.push_back(std::move(m));
    }
    hydrateLabelIds(accountId, out);
    return out;
}

fc::Message MessageRepository::byId(const QString& accountId, const QString& id) {
    auto db = databaseHandle();
    if (accountId.isEmpty()) return {};
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT * FROM messages WHERE account_id = :a AND id = :id"));
    q.bindValue(QStringLiteral(":a"),  accountId);
    q.bindValue(QStringLiteral(":id"), id);
    if (q.exec() && q.next()) {
        auto m = rowToMessage(accountId, q);
        m.attachments = AttachmentRepository::byMessage(accountId, m.id);
        return m;
    }
    return {};
}

std::vector<fc::Message> MessageRepository::byThread(
        const QString& accountId, const QString& threadId) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;
    if (threadId.isEmpty() || accountId.isEmpty()) return out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT * FROM messages WHERE account_id = :a AND thread_id = :t "
        "ORDER BY internal_date ASC"));
    q.bindValue(QStringLiteral(":a"), accountId);
    q.bindValue(QStringLiteral(":t"), threadId);
    if (!q.exec()) return out;
    while (q.next()) out.push_back(rowToMessage(accountId, q));
    for (auto& m : out) {
        m.attachments = AttachmentRepository::byMessage(accountId, m.id);
    }
    hydrateLabelIds(accountId, out);
    return out;
}

bool MessageRepository::exists(const QString& accountId, const QString& id) {
    auto db = databaseHandle();
    if (accountId.isEmpty()) return false;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT 1 FROM messages WHERE account_id = :a AND id = :id"));
    q.bindValue(QStringLiteral(":a"),  accountId);
    q.bindValue(QStringLiteral(":id"), id);
    return q.exec() && q.next();
}

void MessageRepository::applyLabelDiff(const QString& accountId,
                                       const QString& messageId,
                                       const QStringList& added,
                                       const QStringList& removed) {
    if (accountId.isEmpty()) return;
    auto db = databaseHandle();
    // Wrap the three-statement label-edge update in a single
    // transaction so a crash mid-call doesn't leave the cache in
    // a state where the edges and the derived is_unread / is_starred
    // flags disagree. Without the transaction a partial failure
    // produced an inbox where a row was sometimes shown as unread
    // even though UNREAD had just been removed, until the next
    // sync recomputed.
    //
    // If begin-transaction itself fails, abandon the operation
    // rather than fall through to non-atomic writes — the whole
    // point of the transaction was to avoid the half-committed
    // state. The next user action will retry.
    if (!db.transaction()) {
        qWarning("applyLabelDiff: failed to begin txn: %s",
                 qUtf8Printable(db.lastError().text()));
        return;
    }

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO message_labels(account_id, message_id, label_id) "
        "VALUES(:a, :m, :l)"));
    for (const auto& l : added) {
        ins.bindValue(QStringLiteral(":a"), accountId);
        ins.bindValue(QStringLiteral(":m"), messageId);
        ins.bindValue(QStringLiteral(":l"), l);
        if (!ins.exec()) {
            qWarning("applyLabelDiff add %s/%s/%s: %s",
                     qUtf8Printable(accountId), qUtf8Printable(messageId),
                     qUtf8Printable(l),
                     qUtf8Printable(ins.lastError().text()));
        }
    }

    QSqlQuery del(db);
    del.prepare(QStringLiteral(
        "DELETE FROM message_labels "
        "WHERE account_id = :a AND message_id = :m AND label_id = :l"));
    for (const auto& l : removed) {
        del.bindValue(QStringLiteral(":a"), accountId);
        del.bindValue(QStringLiteral(":m"), messageId);
        del.bindValue(QStringLiteral(":l"), l);
        if (!del.exec()) {
            qWarning("applyLabelDiff del %s/%s/%s: %s",
                     qUtf8Printable(accountId), qUtf8Printable(messageId),
                     qUtf8Printable(l),
                     qUtf8Printable(del.lastError().text()));
        }
    }

    QSqlQuery flags(db);
    flags.prepare(QStringLiteral(
        "UPDATE messages SET "
        "  is_unread  = (SELECT COUNT(*) FROM message_labels "
        "                 WHERE account_id = :a AND message_id = :m AND label_id = 'UNREAD')  > 0, "
        "  is_starred = (SELECT COUNT(*) FROM message_labels "
        "                 WHERE account_id = :a AND message_id = :m AND label_id = 'STARRED') > 0 "
        "WHERE account_id = :a AND id = :m"));
    flags.bindValue(QStringLiteral(":a"), accountId);
    flags.bindValue(QStringLiteral(":m"), messageId);
    if (!flags.exec()) {
        qWarning("applyLabelDiff flags %s/%s: %s",
                 qUtf8Printable(accountId), qUtf8Printable(messageId),
                 qUtf8Printable(flags.lastError().text()));
    }

    if (!db.commit()) {
        qWarning("applyLabelDiff: commit failed: %s",
                 qUtf8Printable(db.lastError().text()));
        db.rollback();
    }
}

void MessageRepository::markAccessed(const QString& accountId, const QString& id) {
    if (accountId.isEmpty()) return;
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE messages SET last_accessed_at = :t "
        "WHERE account_id = :a AND id = :id"));
    q.bindValue(QStringLiteral(":t"),  QDateTime::currentMSecsSinceEpoch());
    q.bindValue(QStringLiteral(":a"),  accountId);
    q.bindValue(QStringLiteral(":id"), id);
    q.exec();
}

void MessageRepository::setSnoozeUntil(const QString& accountId,
                                       const QString& id, qint64 wakeAtMs) {
    if (accountId.isEmpty()) return;
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE messages SET snooze_until = :t "
        "WHERE account_id = :a AND id = :id"));
    if (wakeAtMs > 0) {
        q.bindValue(QStringLiteral(":t"), wakeAtMs);
    } else {
        q.bindValue(QStringLiteral(":t"),
                    QVariant(QMetaType(QMetaType::LongLong)));
    }
    q.bindValue(QStringLiteral(":a"),  accountId);
    q.bindValue(QStringLiteral(":id"), id);
    q.exec();
}

QStringList MessageRepository::dueSnoozeWakeups(const QString& accountId) {
    auto db = databaseHandle();
    QStringList out;
    if (accountId.isEmpty()) return out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id FROM messages "
        "WHERE account_id = :a "
        "  AND snooze_until IS NOT NULL "
        "  AND snooze_until <= :now"));
    q.bindValue(QStringLiteral(":a"),   accountId);
    q.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) return out;
    while (q.next()) out << q.value(0).toString();
    return out;
}

// ---------- Cross-account API (v2) ----------

std::vector<fc::Message> MessageRepository::listByLabelAllAccounts(
        const QString& labelId, int limit, int offset) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;

    QSqlQuery q(db);
    // Cross-account: don't filter by m.account_id. The composite-key
    // join still constrains label edges to their own account so a
    // shared label id (e.g. "INBOX") only matches edges scoped to the
    // same row's account_id.
    q.prepare(QStringLiteral(
        "SELECT m.* FROM messages m "
        "JOIN message_labels ml "
        "  ON ml.account_id = m.account_id AND ml.message_id = m.id "
        "WHERE ml.label_id = :l "
        "ORDER BY m.internal_date DESC "
        "LIMIT :lim OFFSET :off"));
    q.bindValue(QStringLiteral(":l"),   labelId);
    q.bindValue(QStringLiteral(":lim"), limit);
    q.bindValue(QStringLiteral(":off"), offset);
    if (!q.exec()) return out;
    while (q.next()) out.push_back(rowToMessage(QString(), q));
    hydrateLabelIds(QString(), out);
    return out;
}

std::vector<fc::Message> MessageRepository::listThreadsByLabelAllAccounts(
        const QString& labelId, int limit, int offset) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;

    QSqlQuery q(db);
    q.prepare(buildThreadRollupSql(QStringLiteral(
        "JOIN message_labels ml "
        "  ON ml.account_id = m.account_id AND ml.message_id = m.id "
        "WHERE ml.label_id = :l")));
    q.bindValue(QStringLiteral(":l"),   labelId);
    q.bindValue(QStringLiteral(":lim"), limit);
    q.bindValue(QStringLiteral(":off"), offset);
    if (!q.exec()) {
        qWarning("listThreadsByLabelAllAccounts: %s",
                 qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) {
        auto m = rowToMessage(QString(), q);
        hydrateThreadAggregates(q, m);
        out.push_back(std::move(m));
    }
    hydrateLabelIds(QString(), out);
    return out;
}

std::vector<fc::Message> MessageRepository::searchFtsAllAccounts(
        const QString& query, int limit) {
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
        qWarning("FTS search (all accounts) failed: %s",
                 qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) out.push_back(rowToMessage(QString(), q));
    hydrateLabelIds(QString(), out);
    return out;
}

std::vector<fc::Message> MessageRepository::searchFtsThreadsAllAccounts(
        const QString& query, int limit) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;
    const QString fts = normaliseFtsQuery(query);
    if (fts.isEmpty()) return out;

    QSqlQuery q(db);
    q.prepare(buildThreadRollupSql(QStringLiteral(
        "JOIN messages_fts f ON f.rowid = m.rowid "
        "WHERE messages_fts MATCH :q")));
    q.bindValue(QStringLiteral(":q"),   fts);
    q.bindValue(QStringLiteral(":lim"), limit);
    q.bindValue(QStringLiteral(":off"), 0);
    if (!q.exec()) {
        qWarning("FTS thread search (all accounts) failed: %s",
                 qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) {
        auto m = rowToMessage(QString(), q);
        hydrateThreadAggregates(q, m);
        out.push_back(std::move(m));
    }
    hydrateLabelIds(QString(), out);
    return out;
}

// ---------- Legacy zero-arg overloads ----------

qint64 MessageRepository::upsert(const fc::Message& m) {
    const QString aid = m.accountId.isEmpty()
        ? Database::defaultAccountId()
        : m.accountId;
    return upsert(aid, m);
}
std::vector<fc::Message> MessageRepository::listByLabel(const QString& labelId,
                                                        int limit, int offset,
                                                        bool unreadOnly) {
    return listByLabel(Database::defaultAccountId(), labelId, limit, offset,
                        unreadOnly);
}
std::vector<fc::Message> MessageRepository::listThreadsByLabel(
        const QString& labelId, int limit, int offset, bool unreadOnly) {
    return listThreadsByLabel(Database::defaultAccountId(), labelId,
                               limit, offset, unreadOnly);
}
std::vector<fc::Message> MessageRepository::searchFts(const QString& query, int limit) {
    return searchFts(Database::defaultAccountId(), query, limit);
}
std::vector<fc::Message> MessageRepository::searchFtsThreads(const QString& query, int limit) {
    return searchFtsThreads(Database::defaultAccountId(), query, limit);
}
fc::Message MessageRepository::byId(const QString& id) {
    return byId(Database::defaultAccountId(), id);
}
bool MessageRepository::exists(const QString& id) {
    return exists(Database::defaultAccountId(), id);
}
std::vector<fc::Message> MessageRepository::byThread(const QString& threadId) {
    return byThread(Database::defaultAccountId(), threadId);
}
void MessageRepository::applyLabelDiff(const QString& messageId,
                                       const QStringList& added,
                                       const QStringList& removed) {
    // Legacy zero-arg overload — route through the per-account
    // version, which carries the transaction-wrap + error logging.
    applyLabelDiff(Database::defaultAccountId(), messageId, added, removed);
}
void MessageRepository::markAccessed(const QString& id) {
    markAccessed(Database::defaultAccountId(), id);
}
void MessageRepository::setSnoozeUntil(const QString& id, qint64 wakeAtMs) {
    setSnoozeUntil(Database::defaultAccountId(), id, wakeAtMs);
}
QStringList MessageRepository::dueSnoozeWakeups() {
    return dueSnoozeWakeups(Database::defaultAccountId());
}

}  // namespace fc::cache
