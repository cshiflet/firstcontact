#include "TokenStore.h"

#include "cache/Database.h"

#include <qt6keychain/keychain.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

using QKeychain::DeletePasswordJob;
using QKeychain::Error;
using QKeychain::ReadPasswordJob;
using QKeychain::WritePasswordJob;

namespace fc::auth {

// ---- key helpers ----

QString TokenStore::serviceName() {
    return QStringLiteral("com.firstcontact.gmail");
}

QString TokenStore::slotKey(const QString& accountId) {
    return QStringLiteral("account:") + accountId;
}

QString TokenStore::indexKey() {
    return QStringLiteral("account-index");
}

QString TokenStore::legacyKey() {
    // Single-account v0 (pre-multi-account) wrote everything under
    // this key. The first launch on a v6+ schema migrates it into a
    // keyed slot, so this only matters for the one-shot upgrade path.
    return QStringLiteral("primary");
}

TokenStore::TokenStore(QObject* parent) : QObject(parent) {}

// ---- index helpers ----

void TokenStore::readIndex(
        std::function<void(QStringList, bool, QString)> cb) {
    auto* job = new ReadPasswordJob(serviceName(), this);
    job->setKey(indexKey());
    connect(job, &ReadPasswordJob::finished, this,
            [cb = std::move(cb)](QKeychain::Job* j) {
        auto* r = static_cast<ReadPasswordJob*>(j);
        if (r->error() == Error::EntryNotFound) {
            cb({}, true, {});
            return;
        }
        if (r->error() != Error::NoError) {
            cb({}, false, r->errorString());
            return;
        }
        QStringList ids;
        const auto doc = QJsonDocument::fromJson(r->textData().toUtf8());
        for (const auto v : doc.array()) ids << v.toString();
        cb(ids, true, {});
    });
    job->start();
}

void TokenStore::writeIndex(const QStringList& ids, DoneCb cb) {
    QJsonArray a;
    for (const auto& id : ids) a.append(id);
    auto* job = new WritePasswordJob(serviceName(), this);
    job->setKey(indexKey());
    job->setTextData(QString::fromUtf8(
        QJsonDocument(a).toJson(QJsonDocument::Compact)));
    connect(job, &WritePasswordJob::finished, this,
            [cb = std::move(cb)](QKeychain::Job* j) {
        auto* w = static_cast<WritePasswordJob*>(j);
        cb(w->error() == Error::NoError, w->errorString());
    });
    job->start();
}

// ---- migrate legacy "primary" slot ----

void TokenStore::migrateLegacyIfPresent(std::function<void()> cb) {
    if (legacyMigrationAttempted_) {
        cb();
        return;
    }
    legacyMigrationAttempted_ = true;

    QPointer<TokenStore> self(this);
    auto* job = new ReadPasswordJob(serviceName(), this);
    job->setKey(legacyKey());
    connect(job, &ReadPasswordJob::finished, this,
            [self, cb = std::move(cb)](QKeychain::Job* j) {
        auto* r = static_cast<ReadPasswordJob*>(j);
        if (!self || r->error() == Error::EntryNotFound) {
            cb();
            return;
        }
        if (r->error() != Error::NoError) {
            qWarning("TokenStore: legacy read failed: %s",
                     qUtf8Printable(r->errorString()));
            cb();
            return;
        }
        const auto doc = QJsonDocument::fromJson(r->textData().toUtf8());
        const auto o = doc.object();

        Tokens t;
        // The legacy slot doesn't carry an accountId. Use the schema's
        // deterministic legacy seed id as a stable target — the same id
        // 0006_multi_account.sql stamped onto every existing cache row.
        t.accountId      = QStringLiteral("00000000-0000-4000-8000-000000000001");
        t.accessToken    = o.value(QStringLiteral("access_token")).toString();
        t.refreshToken   = o.value(QStringLiteral("refresh_token")).toString();
        t.expiresAtUnix  = static_cast<qint64>(
            o.value(QStringLiteral("expires_at")).toDouble());
        t.accountEmail   = o.value(QStringLiteral("email")).toString();

        if (!t.valid()) {
            // Empty legacy slot — drop it.
            auto* del = new DeletePasswordJob(serviceName(), self);
            del->setKey(legacyKey());
            connect(del, &DeletePasswordJob::finished, self,
                    [cb](QKeychain::Job*) { cb(); });
            del->start();
            return;
        }

        // Save the migrated slot, then update the index, then drop legacy.
        self->save(t, [self, cb, accountId = t.accountId](bool ok, QString) {
            Q_UNUSED(ok);
            if (!self) { cb(); return; }
            auto* del = new DeletePasswordJob(serviceName(), self);
            del->setKey(legacyKey());
            connect(del, &DeletePasswordJob::finished, self,
                    [cb](QKeychain::Job*) { cb(); });
            del->start();
        });
    });
    job->start();
}

// ---- per-account load ----

void TokenStore::load(const QString& accountId, LoadOneCb cb) {
    if (accountId.isEmpty()) {
        cb(true, {}, {});
        return;
    }
    auto* job = new ReadPasswordJob(serviceName(), this);
    job->setKey(slotKey(accountId));
    connect(job, &ReadPasswordJob::finished, this,
            [cb = std::move(cb), accountId](QKeychain::Job* j) {
        auto* r = static_cast<ReadPasswordJob*>(j);
        if (r->error() == Error::EntryNotFound) {
            cb(true, {}, {});
            return;
        }
        if (r->error() != Error::NoError) {
            cb(false, {}, r->errorString());
            return;
        }
        const auto doc = QJsonDocument::fromJson(r->textData().toUtf8());
        const auto o = doc.object();
        Tokens t;
        t.accountId      = accountId;
        t.accessToken    = o.value(QStringLiteral("access_token")).toString();
        t.refreshToken   = o.value(QStringLiteral("refresh_token")).toString();
        t.expiresAtUnix  = static_cast<qint64>(
            o.value(QStringLiteral("expires_at")).toDouble());
        t.accountEmail   = o.value(QStringLiteral("email")).toString();
        cb(true, t, {});
    });
    job->start();
}

// ---- loadAll ----

void TokenStore::loadAll(LoadAllCb cb) {
    QPointer<TokenStore> self(this);
    migrateLegacyIfPresent([self, cb = std::move(cb)]() mutable {
        if (!self) { cb(false, {}, QStringLiteral("destroyed")); return; }
        self->readIndex([self, cb = std::move(cb)]
                        (QStringList ids, bool ok, QString err) {
            if (!self) { cb(false, {}, QStringLiteral("destroyed")); return; }
            if (!ok) { cb(false, {}, err); return; }
            if (ids.isEmpty()) {
                cb(true, {}, {});
                return;
            }
            // Issue N reads serially. We could parallelise but the
            // typical N is 1-3 and serial keeps the error reporting
            // straightforward.
            auto outcomes = std::make_shared<QList<Tokens>>();
            auto remaining = std::make_shared<int>(ids.size());
            auto firstError = std::make_shared<QString>();

            for (const auto& id : ids) {
                self->load(id, [outcomes, remaining, firstError, cb]
                          (bool subOk, Tokens t, QString subErr) {
                    if (!subOk && firstError->isEmpty()) {
                        *firstError = subErr;
                    } else if (subOk && !t.accountId.isEmpty()) {
                        outcomes->append(t);
                    }
                    if (--(*remaining) > 0) return;
                    cb(firstError->isEmpty(), *outcomes, *firstError);
                });
            }
        });
    });
}

// ---- save ----

void TokenStore::save(const Tokens& t, DoneCb cb) {
    if (t.accountId.isEmpty()) {
        cb(false, QStringLiteral("save: empty accountId"));
        return;
    }

    QJsonObject o{
        {QStringLiteral("access_token"),  t.accessToken},
        {QStringLiteral("refresh_token"), t.refreshToken},
        {QStringLiteral("expires_at"),    static_cast<double>(t.expiresAtUnix)},
        {QStringLiteral("email"),         t.accountEmail},
    };

    QPointer<TokenStore> self(this);
    auto* job = new WritePasswordJob(serviceName(), this);
    job->setKey(slotKey(t.accountId));
    job->setTextData(QString::fromUtf8(
        QJsonDocument(o).toJson(QJsonDocument::Compact)));
    connect(job, &WritePasswordJob::finished, this,
            [self, accountId = t.accountId, cb = std::move(cb)]
            (QKeychain::Job* j) mutable {
        auto* w = static_cast<WritePasswordJob*>(j);
        if (w->error() != Error::NoError) {
            cb(false, w->errorString());
            return;
        }
        if (!self) { cb(true, {}); return; }
        self->readIndex([self, accountId, cb = std::move(cb)]
                        (QStringList ids, bool ok, QString err) {
            if (!ok) { cb(false, err); return; }
            if (!ids.contains(accountId)) ids.append(accountId);
            if (!self) { cb(true, {}); return; }
            self->writeIndex(ids, std::move(cb));
        });
    });
    job->start();
}

// ---- erase ----

void TokenStore::erase(const QString& accountId, DoneCb cb) {
    if (accountId.isEmpty()) {
        cb(true, {});
        return;
    }
    QPointer<TokenStore> self(this);
    auto* job = new DeletePasswordJob(serviceName(), this);
    job->setKey(slotKey(accountId));
    connect(job, &DeletePasswordJob::finished, this,
            [self, accountId, cb = std::move(cb)](QKeychain::Job* j) mutable {
        auto* d = static_cast<DeletePasswordJob*>(j);
        if (d->error() != Error::NoError &&
            d->error() != Error::EntryNotFound) {
            cb(false, d->errorString());
            return;
        }
        if (!self) { cb(true, {}); return; }
        self->readIndex([self, accountId, cb = std::move(cb)]
                        (QStringList ids, bool ok, QString err) {
            if (!ok) { cb(false, err); return; }
            ids.removeAll(accountId);
            if (!self) { cb(true, {}); return; }
            self->writeIndex(ids, std::move(cb));
        });
    });
    job->start();
}

// ---- legacy zero-arg overloads ----

void TokenStore::load(LoadOneCb cb) {
    const QString aid = fc::cache::Database::defaultAccountId();
    load(aid, std::move(cb));
}

void TokenStore::erase(DoneCb cb) {
    const QString aid = fc::cache::Database::defaultAccountId();
    erase(aid, std::move(cb));
}

}  // namespace fc::auth
