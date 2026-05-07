#include "AccountManager.h"

#include "cache/Database.h"
#include "cache/Migrations.h"

#include <QDateTime>
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
    if (currentId_ == id) {
        currentId_.clear();
        emit currentAccountChanged(currentId_);
    }
    reload();
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

AccountInfo AccountManager::accountById(const QString& id) const {
    for (const auto& a : accounts_) {
        if (a.id == id) return a;
    }
    return {};
}

void AccountManager::selectInitialCurrent() {
    // Same selection rule as Database::defaultAccountId() — pick the
    // is_default row, fall back to most-recently-used, then sort_order.
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
