#include "AccountManager.h"

#include "AccountContext.h"
#include "auth/ClientConfig.h"
#include "auth/OAuthClient.h"
#include "auth/TokenStore.h"
#include "cache/Database.h"
#include "cache/MessageRepository.h"
#include "cache/Migrations.h"
#include "sync/SyncService.h"

#include <QDateTime>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace fc::account {

namespace {

QString mintAccountId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

}  // namespace

AccountManager::AccountManager(QObject* parent) : QObject(parent) {
    fc::cache::Database::initialize();
    reload();
    selectInitialCurrent();
}

AccountManager::AccountManager(fc::auth::ClientConfig* config,
                                fc::auth::TokenStore*   tokenStore,
                                QObject* parent)
    : QObject(parent), config_(config), tokenStore_(tokenStore) {
    fc::cache::Database::initialize();
    reload();
    buildContextsForAllAccounts();
    selectInitialCurrent();
}

AccountManager::~AccountManager() {
    // QObject parenting on contexts handles cleanup, but we drop
    // entries from the hash explicitly so a future "rebuild from
    // scratch" path doesn't see stale pointers.
    contexts_.clear();
}

void AccountManager::buildContextsForAllAccounts() {
    if (!config_ || !tokenStore_) return;
    for (const auto& a : accounts_) {
        if (contexts_.contains(a.id)) continue;
        auto* ctx = new AccountContext(a.id, config_, tokenStore_, this);
        contexts_.insert(a.id, ctx);
        // Re-emit each context's per-account sync signals tagged with
        // the account id so a single connection point in MainWindow
        // reaches every account.
        if (auto* s = ctx->sync()) {
            const QString aid = a.id;
            connect(s, &fc::sync::SyncService::labelsUpdated, this,
                    [this, aid] { emit labelsUpdated(aid); });
            connect(s, &fc::sync::SyncService::messagesUpdated, this,
                    [this, aid] { emit messagesUpdated(aid); });
            connect(s, &fc::sync::SyncService::newMessages, this,
                    [this, aid](int count) { emit newMessages(aid, count); });
            connect(s, &fc::sync::SyncService::failed, this,
                    [this, aid](const QString& reason) {
                        emit syncFailed(aid, reason);
                    });
            connect(s, &fc::sync::SyncService::topUpStarted, this,
                    [this, aid](const QString& labelId) {
                        emit topUpStarted(aid, labelId);
                    });
            connect(s, &fc::sync::SyncService::topUpFinished, this,
                    [this, aid](const QString& labelId, int newRows,
                                bool serverExhausted) {
                        emit topUpFinished(aid, labelId, newRows,
                                           serverExhausted);
                    });
            // Coarse-grained syncStarted/syncFinished: fires on
            // every Idle ↔ non-Idle transition so the UI can show
            // "Syncing…" feedback for initial / incremental syncs
            // (not just top-ups, which only fire for explicit
            // topUpLabel calls).
            connect(s, &fc::sync::SyncService::stateChanged, this,
                    [this, aid](fc::sync::SyncService::State st) {
                        if (st == fc::sync::SyncService::State::Idle)
                            emit syncFinished(aid);
                        else
                            emit syncStarted(aid);
                    });
            connect(s, &fc::sync::SyncService::compressionPromptDue, this,
                    [this, aid](int bodyCount) {
                        emit compressionPromptDue(aid, bodyCount);
                    });
        }
        if (auto* a_oauth = ctx->auth()) {
            const QString aid = a.id;
            connect(a_oauth, &fc::auth::OAuthClient::tokensLoaded, this,
                    [this, aid] { emit tokensLoaded(aid); });
            connect(a_oauth, &fc::auth::OAuthClient::signedOut, this,
                    [this, aid] { emit accountSignedOut(aid); });
        }
    }
}

AccountContext* AccountManager::contextFor(const QString& id) const {
    return contexts_.value(id, nullptr);
}

AccountContext* AccountManager::currentContext() const {
    return contextFor(currentId_);
}

QList<AccountContext*> AccountManager::allContexts() const {
    QList<AccountContext*> out;
    out.reserve(contexts_.size());
    for (auto* c : contexts_) out << c;
    return out;
}

AccountContext* AccountManager::ensureContext(const QString& id) {
    if (id.isEmpty() || !config_ || !tokenStore_) return nullptr;
    if (auto* existing = contexts_.value(id, nullptr)) return existing;
    auto* ctx = new AccountContext(id, config_, tokenStore_, this);
    contexts_.insert(id, ctx);
    // Same per-context signal forwarding that buildContextsForAllAccounts
    // installs at startup. Without this, contexts created mid-session
    // (Add-account flow) would never propagate sync / token events out
    // through AccountManager — the toolbar / sidebar would go quiet
    // until the next launch.
    if (auto* s = ctx->sync()) {
        connect(s, &fc::sync::SyncService::labelsUpdated, this,
                [this, id] { emit labelsUpdated(id); });
        connect(s, &fc::sync::SyncService::messagesUpdated, this,
                [this, id] { emit messagesUpdated(id); });
        connect(s, &fc::sync::SyncService::newMessages, this,
                [this, id](int count) { emit newMessages(id, count); });
        connect(s, &fc::sync::SyncService::failed, this,
                [this, id](const QString& reason) {
                    emit syncFailed(id, reason);
                });
        connect(s, &fc::sync::SyncService::topUpStarted, this,
                [this, id](const QString& labelId) {
                    emit topUpStarted(id, labelId);
                });
        connect(s, &fc::sync::SyncService::topUpFinished, this,
                [this, id](const QString& labelId, int newRows,
                            bool serverExhausted) {
                    emit topUpFinished(id, labelId, newRows, serverExhausted);
                });
        connect(s, &fc::sync::SyncService::stateChanged, this,
                [this, id](fc::sync::SyncService::State st) {
                    if (st == fc::sync::SyncService::State::Idle)
                        emit syncFinished(id);
                    else
                        emit syncStarted(id);
                });
        connect(s, &fc::sync::SyncService::compressionPromptDue, this,
                [this, id](int bodyCount) {
                    emit compressionPromptDue(id, bodyCount);
                });
    }
    if (auto* a_oauth = ctx->auth()) {
        connect(a_oauth, &fc::auth::OAuthClient::tokensLoaded, this,
                [this, id] { emit tokensLoaded(id); });
        connect(a_oauth, &fc::auth::OAuthClient::signedOut, this,
                [this, id] { emit accountSignedOut(id); });
    }
    return ctx;
}

void AccountManager::reload() {
    auto db = fc::cache::databaseHandle();
    accounts_.clear();
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT id, email, display_name, color_hint, sort_order, "
            "       created_at, last_used_at, is_default "
            "FROM accounts ORDER BY sort_order, email"))) {
        qWarning("AccountManager::reload: %s",
                 qUtf8Printable(q.lastError().text()));
        return;
    }
    while (q.next()) {
        AccountInfo a;
        a.id          = q.value(0).toString();
        a.email       = q.value(1).toString();
        a.displayName = q.value(2).toString();
        a.colorHint   = q.value(3).toString();
        a.sortOrder   = q.value(4).toInt();
        a.createdAt   = q.value(5).toLongLong();
        a.lastUsedAt  = q.value(6).toLongLong();
        a.isDefault   = q.value(7).toInt() != 0;
        accounts_.append(std::move(a));
    }
    emit accountsChanged();
}

QList<AccountInfo> AccountManager::accounts() const { return accounts_; }

QString AccountManager::currentAccountId() const { return currentId_; }

void AccountManager::setCurrentAccountId(const QString& id) {
    if (id == currentId_) return;
    // Validate: id must reference an existing accounts row, or be empty
    // (the no-account-signed-in state). Foreign callers may pass stale
    // ids during shutdown / sign-out races.
    if (!id.isEmpty()) {
        bool found = false;
        for (const auto& a : accounts_) {
            if (a.id == id) { found = true; break; }
        }
        if (!found) {
            qWarning("AccountManager::setCurrentAccountId: unknown id '%s'",
                     qUtf8Printable(id));
            return;
        }
    }
    currentId_ = id;
    if (!id.isEmpty()) markUsed(id);
    emit currentAccountChanged(id);
}

QString AccountManager::add(const QString& email,
                            const QString& displayName) {
    if (email.isEmpty()) return {};

    // Idempotent: if an accounts row with this email already exists,
    // return its id. Re-running the OAuth flow against the same address
    // (e.g. a user re-granting consent) shouldn't mint a second row.
    auto db = fc::cache::databaseHandle();
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT id FROM accounts WHERE email = :em"));
        q.bindValue(QStringLiteral(":em"), email);
        if (q.exec() && q.next()) {
            const QString existing = q.value(0).toString();
            if (!displayName.isEmpty()) {
                QSqlQuery upd(db);
                upd.prepare(QStringLiteral(
                    "UPDATE accounts SET display_name = :dn WHERE id = :id"));
                upd.bindValue(QStringLiteral(":dn"), displayName);
                upd.bindValue(QStringLiteral(":id"), existing);
                upd.exec();
            }
            reload();
            return existing;
        }
    }

    const QString id = mintAccountId();
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO accounts(id, email, display_name, sort_order, "
        "                      created_at, is_default) "
        "VALUES(:id, :em, :dn, "
        "       COALESCE((SELECT MAX(sort_order) + 1 FROM accounts), 0), "
        "       :now, "
        "       (SELECT CASE WHEN COUNT(*) = 0 THEN 1 ELSE 0 END FROM accounts))"));
    ins.bindValue(QStringLiteral(":id"),  id);
    ins.bindValue(QStringLiteral(":em"),  email);
    ins.bindValue(QStringLiteral(":dn"),  displayName);
    ins.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
    if (!ins.exec()) {
        qWarning("AccountManager::add(%s): %s",
                 qUtf8Printable(email),
                 qUtf8Printable(ins.lastError().text()));
        return {};
    }
    reload();
    if (config_ && tokenStore_) ensureContext(id);
    return id;
}

bool AccountManager::remove(const QString& id) {
    auto db = fc::cache::databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM accounts WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec()) {
        qWarning("AccountManager::remove(%s): %s",
                 qUtf8Printable(id),
                 qUtf8Printable(q.lastError().text()));
        return false;
    }
    const bool removed = q.numRowsAffected() > 0;
    if (auto* ctx = contexts_.take(id)) ctx->deleteLater();
    if (currentId_ == id) {
        currentId_.clear();
        emit currentAccountChanged(currentId_);
    }
    reload();
    buildContextsForAllAccounts();
    if (currentId_.isEmpty()) selectInitialCurrent();
    return removed;
}

void AccountManager::setDefault(const QString& id) {
    if (id.isEmpty()) return;
    auto db = fc::cache::databaseHandle();
    QSqlQuery clear(db);
    clear.exec(QStringLiteral("UPDATE accounts SET is_default = 0"));
    QSqlQuery set(db);
    set.prepare(QStringLiteral(
        "UPDATE accounts SET is_default = 1 WHERE id = :id"));
    set.bindValue(QStringLiteral(":id"), id);
    set.exec();
    reload();
}

void AccountManager::markUsed(const QString& id) {
    if (id.isEmpty()) return;
    auto db = fc::cache::databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE accounts SET last_used_at = :t WHERE id = :id"));
    q.bindValue(QStringLiteral(":t"),  QDateTime::currentMSecsSinceEpoch());
    q.bindValue(QStringLiteral(":id"), id);
    q.exec();
    // Update the in-memory copy without a full reload — last_used_at
    // changes on every account switch and a reload would emit
    // accountsChanged() (which forces sidebar / menu repaints).
    for (auto& a : accounts_) {
        if (a.id == id) {
            a.lastUsedAt = QDateTime::currentMSecsSinceEpoch();
            break;
        }
    }
}

bool AccountManager::dropCache(const QString& id) {
    if (id.isEmpty()) return false;
    auto db = fc::cache::databaseHandle();
    if (!db.transaction()) {
        qWarning("AccountManager::dropCache: BEGIN failed: %s",
                 qUtf8Printable(db.lastError().text()));
        return false;
    }

    // Order matters: delete child tables before parents so the
    // composite-FK enforcement (foreign_keys = ON in Database::initialize)
    // doesn't reject any individual statement.
    const QStringList stmts = {
        QStringLiteral("DELETE FROM message_labels WHERE account_id = :a"),
        QStringLiteral("DELETE FROM attachments    WHERE account_id = :a"),
        QStringLiteral("DELETE FROM messages       WHERE account_id = :a"),
        QStringLiteral("DELETE FROM threads        WHERE account_id = :a"),
        QStringLiteral("DELETE FROM labels         WHERE account_id = :a"),
        QStringLiteral("DELETE FROM drafts         WHERE account_id = :a"),
        QStringLiteral("DELETE FROM outbox         WHERE account_id = :a"),
        QStringLiteral("DELETE FROM pending_ops    WHERE account_id = :a"),
        QStringLiteral("DELETE FROM account_meta   WHERE account_id = :a"),
    };
    for (const auto& sql : stmts) {
        QSqlQuery q(db);
        q.prepare(sql);
        q.bindValue(QStringLiteral(":a"), id);
        if (!q.exec()) {
            qWarning("AccountManager::dropCache(%s): %s\nSQL: %s",
                     qUtf8Printable(id),
                     qUtf8Printable(q.lastError().text()),
                     qUtf8Printable(sql));
            db.rollback();
            return false;
        }
    }
    if (!db.commit()) {
        qWarning("AccountManager::dropCache(%s): COMMIT failed: %s",
                 qUtf8Printable(id),
                 qUtf8Printable(db.lastError().text()));
        return false;
    }
    // Bulk delete bypassed the per-row FTS5 maintenance in
    // MessageRepository — sync the index now (rebuilds from the
    // surviving messages, so dropped accounts' tokens vanish).
    fc::cache::MessageRepository::reconcileFtsForAccount(id);
    // Drop the cached compression dictionary too — the dict was
    // trained against the now-deleted bodies.
    fc::cache::MessageRepository::invalidateDictionaryCache(id);
    emit cacheCleared(id);
    return true;
}

QStringList AccountManager::accentPalette() {
    // 8 fixed slots, named by their dominant hue. Must stay stable —
    // accounts.color_hint stores the slug verbatim, and a rename
    // would orphan existing settings.
    return {
        QStringLiteral("blue"),
        QStringLiteral("green"),
        QStringLiteral("red"),
        QStringLiteral("purple"),
        QStringLiteral("orange"),
        QStringLiteral("teal"),
        QStringLiteral("pink"),
        QStringLiteral("yellow"),
    };
}

QColor AccountManager::accentColorFor(const QString& accentSlug) {
    // Tones picked to be readable on both light and dark backgrounds
    // — same palette spirit as Gmail's label colours.
    if (accentSlug == QStringLiteral("blue"))   return QColor(0x42, 0x85, 0xf4);
    if (accentSlug == QStringLiteral("green"))  return QColor(0x34, 0xa8, 0x53);
    if (accentSlug == QStringLiteral("red"))    return QColor(0xea, 0x43, 0x35);
    if (accentSlug == QStringLiteral("purple")) return QColor(0xa1, 0x42, 0xf4);
    if (accentSlug == QStringLiteral("orange")) return QColor(0xf2, 0x8b, 0x30);
    if (accentSlug == QStringLiteral("teal"))   return QColor(0x00, 0xa3, 0xa3);
    if (accentSlug == QStringLiteral("pink"))   return QColor(0xe9, 0x42, 0x95);
    if (accentSlug == QStringLiteral("yellow")) return QColor(0xfa, 0xb9, 0x00);
    return QColor();   // invalid — caller paints the default chrome
}

void AccountManager::setAccentColor(const QString& id,
                                     const QString& accentSlug) {
    if (id.isEmpty()) return;
    auto db = fc::cache::databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE accounts SET color_hint = :h WHERE id = :id"));
    if (accentSlug.isEmpty()) {
        q.bindValue(QStringLiteral(":h"),
                    QVariant(QMetaType(QMetaType::QString)));
    } else {
        q.bindValue(QStringLiteral(":h"), accentSlug);
    }
    q.bindValue(QStringLiteral(":id"), id);
    if (q.exec()) reload();
}

qint64 AccountManager::cacheSizeFor(const QString& id) const {
    if (id.isEmpty()) return 0;
    auto db = fc::cache::databaseHandle();

    qint64 total = 0;
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT COALESCE(SUM(bytes_cached), 0) FROM messages "
            "WHERE account_id = :a"));
        q.bindValue(QStringLiteral(":a"), id);
        if (q.exec() && q.next()) total += q.value(0).toLongLong();
    }
    // Attachments — sum of on-disk sizes for downloaded files.
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT local_path FROM attachments "
            "WHERE account_id = :a AND local_path IS NOT NULL"));
        q.bindValue(QStringLiteral(":a"), id);
        if (q.exec()) {
            while (q.next()) {
                QFileInfo fi(q.value(0).toString());
                if (fi.exists()) total += fi.size();
            }
        }
    }
    return total;
}

QStringList AccountManager::orphanedAccountIds() const {
    auto db = fc::cache::databaseHandle();
    QStringList out;
    QSqlQuery q(db);
    // UNION across the per-account child tables, anti-join with
    // accounts. Any account_id that shows up in cache but not in the
    // accounts table is orphaned.
    if (!q.exec(QStringLiteral(
            "SELECT DISTINCT account_id FROM ("
            "  SELECT account_id FROM messages    "
            "  UNION SELECT account_id FROM threads "
            "  UNION SELECT account_id FROM labels "
            "  UNION SELECT account_id FROM drafts "
            "  UNION SELECT account_id FROM outbox "
            "  UNION SELECT account_id FROM pending_ops "
            "  UNION SELECT account_id FROM attachments "
            "  UNION SELECT account_id FROM message_labels "
            "  UNION SELECT account_id FROM account_meta) AS u "
            "WHERE account_id NOT IN (SELECT id FROM accounts) "
            "  AND account_id IS NOT NULL"))) return out;
    while (q.next()) out << q.value(0).toString();
    return out;
}

int AccountManager::dropOrphanedCache() {
    const auto orphans = orphanedAccountIds();
    int dropped = 0;
    for (const auto& id : orphans) {
        if (dropCache(id)) ++dropped;
    }
    return dropped;
}

int AccountManager::clearMessagesOlderThan(const QString& id, int days) {
    if (id.isEmpty() || days <= 0) return 0;
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch()
                        - qint64(days) * 24 * 60 * 60 * 1000LL;
    auto db = fc::cache::databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "DELETE FROM messages "
        "WHERE account_id = :a AND internal_date < :c"));
    q.bindValue(QStringLiteral(":a"), id);
    q.bindValue(QStringLiteral(":c"), cutoff);
    if (!q.exec()) {
        qWarning("clearMessagesOlderThan: %s",
                 qUtf8Printable(q.lastError().text()));
        return 0;
    }
    const int n = q.numRowsAffected();
    if (n > 0) fc::cache::MessageRepository::reconcileFtsForAccount(id);
    return n;
}

int AccountManager::clearMessagesToTargetSize(const QString& id,
                                                qint64 targetBytes) {
    if (id.isEmpty() || targetBytes < 0) return 0;
    const qint64 currentBytes = cacheSizeFor(id);
    if (currentBytes <= targetBytes) return 0;

    auto db = fc::cache::databaseHandle();
    int deleted = 0;
    qint64 freed = 0;
    const qint64 toFree = currentBytes - targetBytes;

    // Walk messages oldest first, accumulating bytes_cached, deleting
    // each row until we've freed enough. We delete one row at a time
    // (rather than computing a cutoff timestamp) because bytes_cached
    // varies wildly per message — a single 50 MB attachment can
    // overshoot a fixed-time cutoff. Loop instead.
    QSqlQuery sel(db);
    sel.prepare(QStringLiteral(
        "SELECT id, COALESCE(bytes_cached, 0) FROM messages "
        "WHERE account_id = :a "
        "ORDER BY internal_date ASC"));
    sel.bindValue(QStringLiteral(":a"), id);
    if (!sel.exec()) {
        qWarning("clearMessagesToTargetSize (select): %s",
                 qUtf8Printable(sel.lastError().text()));
        return 0;
    }

    QStringList toDelete;
    toDelete.reserve(64);
    while (sel.next() && freed < toFree) {
        toDelete << sel.value(0).toString();
        freed += sel.value(1).toLongLong();
    }
    if (toDelete.isEmpty()) return 0;

    // Delete in batches to keep individual statements small.
    constexpr int kBatch = 200;
    for (int i = 0; i < toDelete.size(); i += kBatch) {
        const int upper = qMin(i + kBatch, int(toDelete.size()));
        QStringList placeholders;
        placeholders.reserve(upper - i);
        for (int j = i; j < upper; ++j) {
            placeholders << QStringLiteral("?");
        }
        QSqlQuery del(db);
        del.prepare(QStringLiteral(
            "DELETE FROM messages WHERE account_id = ? AND id IN (%1)")
                        .arg(placeholders.join(QLatin1Char(','))));
        del.addBindValue(id);
        for (int j = i; j < upper; ++j) del.addBindValue(toDelete.at(j));
        if (!del.exec()) {
            qWarning("clearMessagesToTargetSize (delete): %s",
                     qUtf8Printable(del.lastError().text()));
            break;
        }
        deleted += del.numRowsAffected();
    }
    if (deleted > 0) fc::cache::MessageRepository::reconcileFtsForAccount(id);
    return deleted;
}

AccountManager::AccountCacheStats AccountManager::statsFor(
        const QString& id) const {
    AccountCacheStats s;
    if (id.isEmpty()) return s;
    auto db = fc::cache::databaseHandle();

    s.sizeBytes = cacheSizeFor(id);

    auto countOf = [&](const char* table) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM %1 "
                                  "WHERE account_id = :a").arg(
                                      QString::fromLatin1(table)));
        q.bindValue(QStringLiteral(":a"), id);
        if (q.exec() && q.next()) return q.value(0).toInt();
        return 0;
    };
    s.messageCount    = countOf("messages");
    s.threadCount     = countOf("threads");
    s.labelCount      = countOf("labels");
    s.attachmentCount = countOf("attachments");
    s.draftCount      = countOf("drafts");
    s.outboxCount     = countOf("outbox");
    s.pendingOpsCount = countOf("pending_ops");
    return s;
}

AccountInfo AccountManager::accountById(const QString& id) const {
    for (const auto& a : accounts_) {
        if (a.id == id) return a;
    }
    return {};
}

void AccountManager::selectInitialCurrent() {
    // Pick the active account: the is_default row, falling back to
    // most-recently-used, then sort_order, then email.
    if (accounts_.isEmpty()) {
        currentId_.clear();
        return;
    }
    QString picked;
    for (const auto& a : accounts_) {
        if (a.isDefault) { picked = a.id; break; }
    }
    if (picked.isEmpty()) {
        const AccountInfo* best = nullptr;
        for (const auto& a : accounts_) {
            if (!best || a.lastUsedAt > best->lastUsedAt) best = &a;
        }
        if (best) picked = best->id;
    }
    if (picked.isEmpty()) picked = accounts_.first().id;
    currentId_ = picked;
    emit currentAccountChanged(currentId_);
}

}  // namespace fc::account
