#include "TokenStore.h"

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

TokenStore::TokenStore(QObject* parent) : QObject(parent) {}

// ---- index helpers ----

void TokenStore::readIndex(
        std::function<void(QStringList, bool, QString)> cb) {
    // Parent set to nullptr (rather than `this`) so the job is born
    // on whatever thread is calling — TokenStore lives on the UI
    // thread but per-account OAuthClients call us from their own
    // sync threads. A cross-thread parent on a QObject would trip
    // Qt's "QObject: Cannot create children for a parent that is in
    // a different thread" warning. QKeychain::Job::setAutoDelete is
    // on by default so the job still self-destructs after finished
    // fires.
    auto* job = new ReadPasswordJob(serviceName());
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
    auto* job = new WritePasswordJob(serviceName());  // no parent — see readIndex
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

// ---- per-account load ----

void TokenStore::load(const QString& accountId, LoadOneCb cb) {
    if (accountId.isEmpty()) {
        cb(true, {}, {});
        return;
    }
    auto* job = new ReadPasswordJob(serviceName());  // no parent — see readIndex
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
        QJsonParseError parseErr;
        const auto doc = QJsonDocument::fromJson(r->textData().toUtf8(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
            // Treat parse failure as a real error rather than as
            // "no credentials" — the latter would silently push the
            // user back through the consent screen instead of
            // surfacing the corruption. Caller flow (UI) shows
            // a sign-in-failed banner so the user knows something
            // is wrong with their keychain entry.
            cb(false, {},
               QStringLiteral("token store payload is not valid JSON: ") +
                   parseErr.errorString());
            return;
        }
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
    readIndex([self, cb = std::move(cb)]
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
    auto* job = new WritePasswordJob(serviceName());  // no parent — see readIndex
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
    auto* job = new DeletePasswordJob(serviceName());  // no parent — see readIndex
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

}  // namespace fc::auth
