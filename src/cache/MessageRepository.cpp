#include "MessageRepository.h"

#include "AttachmentRepository.h"
#include "Database.h"
#include "Migrations.h"
#include "util/BodyCodec.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
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

// ---------- Per-account compression dictionary cache ----------
//
// Loaded lazily from body_compression_dict on first dictionaryFor()
// call, then reused by every compress/decompress in the process.
// Multiple threads (UI + each sync thread) may call this; guard the
// cache with a mutex. Cache invalidation happens on saveDictionary +
// drop-cache hooks via invalidateDictionaryCache().
QMutex                     g_dictMutex;
QHash<QString, QByteArray> g_dictCache;
QSet<QString>              g_dictLoaded;   // empty dict still counts as "loaded"

QByteArray loadDictFromDb(const QString& accountId) {
    if (accountId.isEmpty()) return {};
    auto db = fc::cache::databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT dict FROM body_compression_dict WHERE account_id = :a"));
    q.bindValue(QStringLiteral(":a"), accountId);
    if (!q.exec() || !q.next()) return {};
    return q.value(0).toByteArray();
}

QByteArray dictFor(const QString& accountId) {
    if (accountId.isEmpty()) return {};
    QMutexLocker lock(&g_dictMutex);
    if (g_dictLoaded.contains(accountId)) {
        return g_dictCache.value(accountId);
    }
    // Drop the lock for the DB read so concurrent threads can still
    // hit the cache for other accounts. A racing parallel load is
    // harmless (one of two identical dictionaries wins the insert).
    lock.unlock();
    QByteArray dict = loadDictFromDb(accountId);
    lock.relock();
    g_dictCache.insert(accountId, dict);
    g_dictLoaded.insert(accountId);
    return dict;
}

// Compress / decompress wrappers that look up the per-account dict
// once. Empty dict (no training yet) leaves payloads as plaintext.
QByteArray compressBody(const QString& accountId, const QString& plain) {
    if (plain.isEmpty()) return {};
    const QByteArray dict = dictFor(accountId);
    if (dict.isEmpty()) return plain.toUtf8();   // pass-through
    return fc::util::BodyCodec::compress(plain.toUtf8(), dict);
}

QString decompressBody(const QString& accountId, const QByteArray& bytes) {
    if (bytes.isEmpty()) return {};
    if (!fc::util::BodyCodec::isCompressed(bytes)) {
        // Plaintext row (legacy or post-pass-through). Interpret as
        // UTF-8 — same semantics as the previous QVariant.toString().
        return QString::fromUtf8(bytes);
    }
    const QByteArray dict = dictFor(accountId);
    return QString::fromUtf8(
        fc::util::BodyCodec::decompress(bytes, dict));
}

// ---------- Programmatic FTS5 maintenance ----------
//
// The 0006 trigger-based maintenance (messages_ai/au/ad) was dropped
// in migration 0008 because triggers can't see plaintext body when
// the column persists compressed bytes. These helpers replicate what
// the triggers did, called from upsertOneNoTxn / single-row delete
// paths. Bulk deletes call reconcileFtsForAccount() instead.

void ftsDeleteByRowid(QSqlDatabase& db,
                       qint64 rowid,
                       const QString& subject,
                       const QString& fromText,
                       const QString& bodyText,
                       const QString& snippet,
                       const QString& accountId) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO messages_fts(messages_fts, rowid, subject, from_text, "
        "                          body, snippet, account_id) "
        "VALUES ('delete', :rowid, :subject, :from_text, :body, :snippet, :a)"));
    q.bindValue(QStringLiteral(":rowid"),     rowid);
    q.bindValue(QStringLiteral(":subject"),   subject);
    q.bindValue(QStringLiteral(":from_text"), fromText);
    q.bindValue(QStringLiteral(":body"),      bodyText);
    q.bindValue(QStringLiteral(":snippet"),   snippet);
    q.bindValue(QStringLiteral(":a"),         accountId);
    if (!q.exec()) {
        qWarning("ftsDeleteByRowid: %s",
                 qUtf8Printable(q.lastError().text()));
    }
}

void ftsInsert(QSqlDatabase& db,
                qint64 rowid,
                const QString& subject,
                const QString& fromText,
                const QString& bodyText,
                const QString& snippet,
                const QString& accountId) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO messages_fts(rowid, subject, from_text, body, "
        "                          snippet, account_id) "
        "VALUES (:rowid, :subject, :from_text, :body, :snippet, :a)"));
    q.bindValue(QStringLiteral(":rowid"),     rowid);
    q.bindValue(QStringLiteral(":subject"),   subject);
    q.bindValue(QStringLiteral(":from_text"), fromText);
    q.bindValue(QStringLiteral(":body"),      bodyText);
    q.bindValue(QStringLiteral(":snippet"),   snippet);
    q.bindValue(QStringLiteral(":a"),         accountId);
    if (!q.exec()) {
        qWarning("ftsInsert: %s", qUtf8Printable(q.lastError().text()));
    }
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
    // body_text / body_html may be stored as compressed BLOB once a
    // dictionary is trained. Always read as bytes + route through the
    // codec — for plaintext (no magic prefix), decompressBody returns
    // the bytes as a UTF-8 QString, same as the legacy
    // QVariant.toString() did.
    m.bodyText        = decompressBody(m.accountId,
                            q.value(QStringLiteral("body_text")).toByteArray());
    m.bodyHtml        = decompressBody(m.accountId,
                            q.value(QStringLiteral("body_html")).toByteArray());
    m.bodyHtmlPresent = q.value(QStringLiteral("body_html_present")).toBool();
    m.isUnread        = q.value(QStringLiteral("is_unread")).toBool();
    m.isStarred       = q.value(QStringLiteral("is_starred")).toBool();
    m.isImportant     = q.value(QStringLiteral("is_important")).toBool();
    m.hasAttachment   = q.value(QStringLiteral("has_attachment")).toBool();
    m.fetchedFormat   = q.value(QStringLiteral("fetched_format")).toString();
    m.bodyCompression = q.value(QStringLiteral("body_compression")).toInt();
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
        "       has_attachment, fetched_format, body_compression, "
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
    m.isUnread       = m.threadHasUnread;
    m.isStarred      = m.threadHasStarred;
    m.hasAttachment  = m.threadHasAttachment;
}

}  // namespace

// Runs the 3-step write (thread + message + label edges + attachments)
// for one message INSIDE the caller's transaction. Returns the
// inserted rowid (>0) on success, 0 on failure (caller decides
// whether to rollback the outer transaction or just skip).
//
// Extracted so the public upsert() can wrap a single transaction
// per call and upsertMany() can wrap one transaction across N calls.
static qint64 upsertOneNoTxn(QSqlDatabase& db, const QString& accountId,
                              const fc::Message& m) {
    // 0. Snapshot the row's pre-update FTS-relevant fields (if it
    // exists) so we can issue an FTS5 'delete' against the old
    // content before re-inserting the new content. With the v0006
    // triggers gone (migration 0008), nothing else does this for us.
    qint64  oldRowid    = -1;
    QString oldSubject, oldFromText, oldBodyText, oldSnippet;
    {
        QSqlQuery snap(db);
        snap.prepare(QStringLiteral(
            "SELECT rowid, subject, from_name, from_addr, body_text, snippet "
            "FROM messages WHERE account_id = :a AND id = :id"));
        snap.bindValue(QStringLiteral(":a"),  accountId);
        snap.bindValue(QStringLiteral(":id"), m.id);
        if (snap.exec() && snap.next()) {
            oldRowid    = snap.value(0).toLongLong();
            oldSubject  = snap.value(1).toString();
            oldFromText = snap.value(2).toString() + QLatin1Char(' ')
                        + snap.value(3).toString();
            // body_text may be compressed bytes; decompress so the
            // FTS5 'delete' sees the same tokens we originally
            // indexed. (FTS5 derives the token list from the
            // supplied content during 'delete'.)
            oldBodyText = decompressBody(accountId,
                              snap.value(4).toByteArray());
            oldSnippet  = snap.value(5).toString();
        }
    }

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
            return 0;
        }
    }

    QSqlQuery q(db);
    // Compress body fields once before binding. Empty dict = identity
    // → bytes are the UTF-8 plaintext, same on-disk shape as before
    // compression landed.
    const QByteArray bodyTextBytes = compressBody(accountId, m.bodyText);
    const QByteArray bodyHtmlBytes = compressBody(accountId, m.bodyHtml);
    const int bodyCompFlag =
        (fc::util::BodyCodec::isCompressed(bodyTextBytes)
         || fc::util::BodyCodec::isCompressed(bodyHtmlBytes))
        ? 1 : 0;

    q.prepare(QStringLiteral(
        "INSERT INTO messages(account_id, id, thread_id, history_id, "
        "  internal_date, size_estimate, from_addr, from_name, to_addrs, "
        "  cc_addrs, bcc_addrs, reply_to, subject, snippet, is_unread, "
        "  is_starred, is_important, has_attachment, body_text, body_html, "
        "  body_html_present, fetched_format, bytes_cached, last_accessed_at, "
        "  body_compression, created_at) "
        "VALUES(:a, :id, :thread_id, :history_id, :internal_date, "
        "  :size_estimate, :from_addr, :from_name, :to_addrs, :cc_addrs, "
        "  :bcc_addrs, :reply_to, :subject, :snippet, :is_unread, :is_starred, "
        "  :is_important, :has_attachment, :body_text, :body_html, "
        "  :body_html_present, :fetched_format, :bytes_cached, "
        "  :last_accessed_at, :body_compression, :created_at) "
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
        // body_text / body_html: only overwrite if the incoming body
        // is non-empty. NULLIF on a BLOB returns NULL when the value
        // equals zero-length blob, which is what we want for the
        // metadata-only upsert path.
        "  body_text      = COALESCE(NULLIF(excluded.body_text, X''), messages.body_text), "
        "  body_html      = COALESCE(NULLIF(excluded.body_html, X''), messages.body_html), "
        "  body_html_present = excluded.body_html_present, "
        "  body_compression  = CASE "
        "    WHEN excluded.body_text IS NULL OR length(excluded.body_text) = 0 "
        "      THEN messages.body_compression "
        "    ELSE excluded.body_compression "
        "  END, "
        "  fetched_format = excluded.fetched_format, "
        // Must follow the same COALESCE/NULLIF merge as body_text /
        // body_html so a metadata-only resync doesn't drop bytes_cached
        // to 0 while the bodies stay preserved.
        "  bytes_cached = "
        "    IFNULL(length(COALESCE(NULLIF(excluded.body_text, X''), "
        "                            messages.body_text)), 0) + "
        "    IFNULL(length(COALESCE(NULLIF(excluded.body_html, X''), "
        "                            messages.body_html)), 0)"));

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
    q.bindValue(QStringLiteral(":body_text"),         bodyTextBytes);
    q.bindValue(QStringLiteral(":body_html"),         bodyHtmlBytes);
    q.bindValue(QStringLiteral(":body_html_present"), m.bodyHtmlPresent ? 1 : 0);
    q.bindValue(QStringLiteral(":fetched_format"),
                m.bodyText.isEmpty() ? QStringLiteral("metadata")
                                     : QStringLiteral("full"));
    // bytes_cached tracks the on-disk footprint (the BLOB size, not
    // the plaintext size) so the Cache Manager dialog's totals
    // reflect what compression actually saved.
    q.bindValue(QStringLiteral(":bytes_cached"),
                bodyTextBytes.size() + bodyHtmlBytes.size());
    q.bindValue(QStringLiteral(":last_accessed_at"), now);
    q.bindValue(QStringLiteral(":body_compression"), bodyCompFlag);
    q.bindValue(QStringLiteral(":created_at"),       now);

    if (!q.exec()) {
        qWarning("MessageRepository::upsert (message): %s",
                 qUtf8Printable(q.lastError().text()));
        return 0;
    }

    // Label edges + attachments (each does its own SQL inside the
    // same outer transaction — these helpers don't open one of
    // their own).
    writeLabelEdges(db, accountId, m.id, m.labelIds);
    AttachmentRepository::replaceForMessage(accountId, m.id, m.attachments);

    // FTS5 maintenance — replicates what the dropped v0006 triggers
    // did. For UPSERTs that updated an existing row, 'delete' the
    // pre-update content first; then INSERT the new content. The
    // body fed to FTS is always the plaintext (m.bodyText), even
    // when the column persists compressed bytes.
    qint64 newRowid = 0;
    {
        QSqlQuery rowidQ(db);
        rowidQ.prepare(QStringLiteral(
            "SELECT rowid FROM messages WHERE account_id = :a AND id = :id"));
        rowidQ.bindValue(QStringLiteral(":a"),  accountId);
        rowidQ.bindValue(QStringLiteral(":id"), m.id);
        if (rowidQ.exec() && rowidQ.next()) {
            newRowid = rowidQ.value(0).toLongLong();
        }
    }
    if (oldRowid >= 0) {
        ftsDeleteByRowid(db, oldRowid, oldSubject, oldFromText,
                         oldBodyText, oldSnippet, accountId);
    }
    if (newRowid > 0) {
        const QString newFromText = m.fromName.isEmpty()
            ? m.fromAddr
            : (m.fromName + QLatin1Char(' ') + m.fromAddr);
        // Only feed body to FTS when the incoming upsert carries it.
        // For metadata-only upserts (m.bodyText empty), we'd lose
        // the previously-indexed body terms by re-inserting an
        // empty body — so on that path, skip the FTS insert; the
        // pre-update FTS row is still there for the body, only the
        // metadata fields are stale.
        if (!m.bodyText.isEmpty() || oldRowid < 0) {
            ftsInsert(db, newRowid, m.subject, newFromText,
                      m.bodyText, m.snippet, accountId);
        } else if (oldRowid >= 0) {
            // Metadata-only upsert: re-insert with the OLD body so
            // body tokens stay indexed, but with the NEW subject /
            // from / snippet so a sender/subject change shows up
            // in search results.
            ftsInsert(db, newRowid, m.subject, newFromText,
                      oldBodyText, m.snippet, accountId);
        }
    }
    return newRowid;
}

int MessageRepository::upsertMany(const QString& accountId,
                                   const std::vector<fc::Message>& msgs) {
    if (accountId.isEmpty()) {
        qWarning("MessageRepository::upsertMany called without an "
                 "accountId; %lld msgs dropped",
                 static_cast<long long>(msgs.size()));
        return 0;
    }
    if (msgs.empty()) return 0;
    auto db = databaseHandle();
    if (!db.transaction()) {
        qWarning("MessageRepository::upsertMany: BEGIN failed: %s",
                 qUtf8Printable(db.lastError().text()));
        return 0;
    }
    int stored = 0;
    for (const auto& m : msgs) {
        if (upsertOneNoTxn(db, accountId, m) > 0) ++stored;
    }
    if (!db.commit()) {
        qWarning("MessageRepository::upsertMany: COMMIT failed: %s",
                 qUtf8Printable(db.lastError().text()));
        db.rollback();
        return 0;
    }
    return stored;
}

qint64 MessageRepository::upsert(const QString& accountId, const fc::Message& m) {
    if (accountId.isEmpty()) {
        qWarning("MessageRepository::upsert called without an accountId; "
                 "msg %s dropped", qUtf8Printable(m.id));
        return 0;
    }
    auto db = databaseHandle();

    // Single-message write: thread row → message → label edges → attachments.
    // Wrapped in a transaction so a failure leaves the cache untouched.
    if (!db.transaction()) {
        qWarning("MessageRepository::upsert: BEGIN failed: %s",
                 qUtf8Printable(db.lastError().text()));
        return 0;
    }
    const qint64 rowid = upsertOneNoTxn(db, accountId, m);
    if (rowid == 0) {
        db.rollback();
        return 0;
    }
    db.commit();
    return rowid;
}

// Legacy single-message implementation retained below for reference;
// kept compiling-clean by inlining its body into upsertOneNoTxn above.
// The old function body that started with db.transaction() at the top
// is fully replaced; the only-remaining duplicate would have been the
// inlined SQL string. Removing the old body:
#if 0
qint64 MessageRepository::upsertOldBody(const QString& accountId, const fc::Message& m) {
    auto db = databaseHandle();
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
        "  fetched_format = excluded.fetched_format, "
        // Mirror the COALESCE/NULLIF merge on body_text / body_html
        // so bytes_cached stays consistent with the preserved bodies.
        "  bytes_cached = "
        "    IFNULL(length(COALESCE(NULLIF(excluded.body_text, ''), "
        "                            messages.body_text)), 0) + "
        "    IFNULL(length(COALESCE(NULLIF(excluded.body_html, ''), "
        "                            messages.body_html)), 0)"));

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
#endif  // legacy upsert body

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

std::vector<fc::Message> MessageRepository::listAllMail(
        const QString& accountId, int limit, int offset, bool unreadOnly) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;
    if (accountId.isEmpty()) return out;

    // "All Mail": every cached message for the account except those
    // carrying SPAM or TRASH. Implemented with NOT EXISTS rather
    // than NOT IN (...) so the predicate is straightforward to
    // index-match against the (account_id, label_id) pair.
    QSqlQuery q(db);
    if (unreadOnly) {
        q.prepare(QStringLiteral(
            "SELECT m.* FROM messages m "
            "WHERE m.account_id = :a AND m.is_unread = 1 "
            "  AND NOT EXISTS ("
            "    SELECT 1 FROM message_labels ml "
            "    WHERE ml.account_id = m.account_id "
            "      AND ml.message_id = m.id "
            "      AND ml.label_id IN ('SPAM', 'TRASH')) "
            "ORDER BY m.internal_date DESC "
            "LIMIT :lim OFFSET :off"));
    } else {
        q.prepare(QStringLiteral(
            "SELECT m.* FROM messages m "
            "WHERE m.account_id = :a "
            "  AND NOT EXISTS ("
            "    SELECT 1 FROM message_labels ml "
            "    WHERE ml.account_id = m.account_id "
            "      AND ml.message_id = m.id "
            "      AND ml.label_id IN ('SPAM', 'TRASH')) "
            "ORDER BY m.internal_date DESC "
            "LIMIT :lim OFFSET :off"));
    }
    q.bindValue(QStringLiteral(":a"),   accountId);
    q.bindValue(QStringLiteral(":lim"), limit);
    q.bindValue(QStringLiteral(":off"), offset);
    if (!q.exec()) {
        qWarning("listAllMail: %s", qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) out.push_back(rowToMessage(accountId, q));
    hydrateLabelIds(accountId, out);
    qInfo("listAllMail(acc=%s, limit=%d, offset=%d): %zu messages",
          qUtf8Printable(accountId), limit, offset, out.size());
    return out;
}

std::vector<fc::Message> MessageRepository::listThreadsAllMail(
        const QString& accountId, int limit, int offset, bool unreadOnly) {
    auto db = databaseHandle();
    std::vector<fc::Message> out;
    if (accountId.isEmpty()) return out;

    // Conversation-view variant. The inner WHERE is the same SPAM/
    // TRASH exclusion; the rollup window selects one row per thread
    // (latest by internal_date) and aggregates unread/starred/attach.
    QString sql = buildThreadRollupSql(QStringLiteral(
        "WHERE m.account_id = :a "
        "  AND NOT EXISTS ("
        "    SELECT 1 FROM message_labels ml "
        "    WHERE ml.account_id = m.account_id "
        "      AND ml.message_id = m.id "
        "      AND ml.label_id IN ('SPAM', 'TRASH'))"));
    if (unreadOnly) {
        sql = QStringLiteral(
            "SELECT * FROM (%1) WHERE t_unread > 0 "
            "ORDER BY internal_date DESC "
            "LIMIT :outerLim OFFSET :outerOff").arg(sql);
    }

    QSqlQuery q(db);
    q.prepare(sql);
    q.bindValue(QStringLiteral(":a"), accountId);
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
        qWarning("listThreadsAllMail: %s",
                 qUtf8Printable(q.lastError().text()));
        return out;
    }
    while (q.next()) {
        auto m = rowToMessage(accountId, q);
        hydrateThreadAggregates(q, m);
        out.push_back(std::move(m));
    }
    hydrateLabelIds(accountId, out);
    qInfo("listThreadsAllMail(acc=%s, limit=%d, offset=%d): %zu threads",
          qUtf8Printable(accountId), limit, offset, out.size());
    return out;
}

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

// ----------------------------------------------------------------------
// Compression-dictionary slot

QByteArray MessageRepository::dictionaryFor(const QString& accountId) {
    return dictFor(accountId);
}

void MessageRepository::saveDictionary(const QString& accountId,
                                        const QByteArray& dict,
                                        int sampleCount,
                                        qint64 sampleBytes) {
    if (accountId.isEmpty()) return;
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO body_compression_dict("
        "    account_id, dict, version, created_at, sample_count, sample_bytes) "
        "VALUES(:a, :d, 1, :t, :sc, :sb) "
        "ON CONFLICT(account_id) DO UPDATE SET "
        "  dict         = excluded.dict, "
        "  version      = excluded.version, "
        "  created_at   = excluded.created_at, "
        "  sample_count = excluded.sample_count, "
        "  sample_bytes = excluded.sample_bytes"));
    q.bindValue(QStringLiteral(":a"),  accountId);
    q.bindValue(QStringLiteral(":d"),  dict);
    q.bindValue(QStringLiteral(":t"),  QDateTime::currentMSecsSinceEpoch());
    q.bindValue(QStringLiteral(":sc"), sampleCount);
    q.bindValue(QStringLiteral(":sb"), sampleBytes);
    if (!q.exec()) {
        qWarning("MessageRepository::saveDictionary (%s): %s",
                 qUtf8Printable(accountId),
                 qUtf8Printable(q.lastError().text()));
        return;
    }
    // Refresh cache so the next compress/decompress sees the new dict.
    QMutexLocker lock(&g_dictMutex);
    g_dictCache.insert(accountId, dict);
    g_dictLoaded.insert(accountId);
}

void MessageRepository::invalidateDictionaryCache(const QString& accountId) {
    QMutexLocker lock(&g_dictMutex);
    if (accountId.isEmpty()) {
        g_dictCache.clear();
        g_dictLoaded.clear();
    } else {
        g_dictCache.remove(accountId);
        g_dictLoaded.remove(accountId);
    }
}

int MessageRepository::bodyCountFor(const QString& accountId) {
    if (accountId.isEmpty()) return 0;
    auto db = databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM messages "
        "WHERE account_id = :a "
        "  AND ( (body_text IS NOT NULL AND length(body_text) > 0) "
        "     OR (body_html IS NOT NULL AND length(body_html) > 0) )"));
    q.bindValue(QStringLiteral(":a"), accountId);
    if (!q.exec() || !q.next()) return 0;
    return q.value(0).toInt();
}

// ----------------------------------------------------------------------
// Bulk FTS reconciliation. Used after wholesale row deletes (dropCache,
// clearMessagesOlderThan, clearMessagesToTargetSize) where doing
// per-row FTS 'delete' would require fetching every row's content
// before the delete.
//
// We can't use FTS5's `'rebuild'` command here: the messages_fts
// virtual table declares columns (subject, from_text, body, snippet)
// that don't correspond 1:1 to messages columns — from_text is
// synthesized from `from_name || ' ' || from_addr`, and body comes
// from body_text (different name) which may be zstd-compressed. The
// dropped v0006 triggers handled the mapping at INSERT time;
// rebuild has no way to.
//
// So: delete-all the FTS index, then walk every surviving message
// and re-insert with the same column mapping the upsert path uses
// (decompressed body, concatenated from_text). Global (across all
// accounts) because FTS5 doesn't support partial 'delete-all'.
// Acceptable since this only fires after bulk deletes.
void MessageRepository::reconcileFtsForAccount(const QString& accountId) {
    Q_UNUSED(accountId);
    auto db = databaseHandle();
    QSqlQuery wipe(db);
    if (!wipe.exec(QStringLiteral(
            "INSERT INTO messages_fts(messages_fts) VALUES('delete-all')"))) {
        qWarning("reconcileFtsForAccount delete-all: %s",
                 qUtf8Printable(wipe.lastError().text()));
        return;
    }
    QSqlQuery sel(db);
    if (!sel.exec(QStringLiteral(
            "SELECT account_id, rowid, "
            "       COALESCE(subject, ''), "
            "       COALESCE(from_name, ''), "
            "       COALESCE(from_addr, ''), "
            "       body_text, "
            "       COALESCE(snippet, '') "
            "FROM messages"))) {
        qWarning("reconcileFtsForAccount select: %s",
                 qUtf8Printable(sel.lastError().text()));
        return;
    }
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO messages_fts(rowid, subject, from_text, body, "
        "                          snippet, account_id) "
        "VALUES (:rowid, :subject, :from_text, :body, :snippet, :a)"));
    int reinserted = 0;
    while (sel.next()) {
        const QString aid     = sel.value(0).toString();
        const qint64  rowid   = sel.value(1).toLongLong();
        const QString subject = sel.value(2).toString();
        const QString fromN   = sel.value(3).toString();
        const QString fromA   = sel.value(4).toString();
        // body_text may be a compressed BLOB; decompress to plaintext
        // before indexing so search queries find the right tokens.
        const QString body    = decompressBody(aid,
                                    sel.value(5).toByteArray());
        const QString snippet = sel.value(6).toString();

        ins.bindValue(QStringLiteral(":rowid"),     rowid);
        ins.bindValue(QStringLiteral(":subject"),   subject);
        ins.bindValue(QStringLiteral(":from_text"),
                       fromN.isEmpty() ? fromA
                                       : (fromN + QLatin1Char(' ') + fromA));
        ins.bindValue(QStringLiteral(":body"),      body);
        ins.bindValue(QStringLiteral(":snippet"),   snippet);
        ins.bindValue(QStringLiteral(":a"),         aid);
        if (!ins.exec()) {
            qWarning("reconcileFtsForAccount insert (rowid=%lld): %s",
                     static_cast<long long>(rowid),
                     qUtf8Printable(ins.lastError().text()));
            continue;
        }
        ++reinserted;
    }
    if (reinserted > 0) {
        qInfo("reconcileFtsForAccount: re-indexed %d row(s) (caller=%s)",
              reinserted, qUtf8Printable(accountId));
    }
}

int MessageRepository::removeFtsForAccount(const QString& accountId) {
    if (accountId.isEmpty()) return 0;
    auto db = databaseHandle();
    QSqlQuery sel(db);
    sel.prepare(QStringLiteral(
        "SELECT rowid, "
        "       COALESCE(subject, ''), "
        "       COALESCE(from_name, ''), "
        "       COALESCE(from_addr, ''), "
        "       body_text, "
        "       COALESCE(snippet, '') "
        "FROM messages WHERE account_id = :a"));
    sel.bindValue(QStringLiteral(":a"), accountId);
    if (!sel.exec()) {
        qWarning("removeFtsForAccount select: %s",
                 qUtf8Printable(sel.lastError().text()));
        return 0;
    }
    int removed = 0;
    while (sel.next()) {
        const qint64  rowid   = sel.value(0).toLongLong();
        const QString subject = sel.value(1).toString();
        const QString fromN   = sel.value(2).toString();
        const QString fromA   = sel.value(3).toString();
        const QString body    = decompressBody(accountId,
                                    sel.value(4).toByteArray());
        const QString snippet = sel.value(5).toString();
        const QString fromText = fromN.isEmpty()
            ? fromA : (fromN + QLatin1Char(' ') + fromA);
        ftsDeleteByRowid(db, rowid, subject, fromText, body, snippet,
                          accountId);
        ++removed;
    }
    if (removed > 0) {
        qInfo("removeFtsForAccount(%s): cleaned %d FTS row(s)",
              qUtf8Printable(accountId), removed);
    }
    return removed;
}

}  // namespace fc::cache
