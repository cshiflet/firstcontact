#include "TokenStore.h"

#include <qt6keychain/keychain.h>

#include <QJsonDocument>
#include <QJsonObject>

using QKeychain::DeletePasswordJob;
using QKeychain::Error;
using QKeychain::ReadPasswordJob;
using QKeychain::WritePasswordJob;

namespace fc::auth {

QString TokenStore::serviceName() {
    return QStringLiteral("com.firstcontact.gmail");
}

QString TokenStore::keyName() {
    // Single-account v1 — one slot. Multi-account would key by email.
    return QStringLiteral("primary");
}

TokenStore::TokenStore(QObject* parent) : QObject(parent) {}

void TokenStore::load(Callback cb) {
    auto* job = new ReadPasswordJob(serviceName(), this);
    job->setKey(keyName());
    connect(job, &ReadPasswordJob::finished, this, [cb = std::move(cb)](QKeychain::Job* j) {
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
        t.accessToken    = o.value(QStringLiteral("access_token")).toString();
        t.refreshToken   = o.value(QStringLiteral("refresh_token")).toString();
        t.expiresAtUnix  = static_cast<qint64>(o.value(QStringLiteral("expires_at")).toDouble());
        t.accountEmail   = o.value(QStringLiteral("email")).toString();
        cb(true, t, {});
    });
    job->start();
}

void TokenStore::save(const Tokens& t, std::function<void(bool, QString)> cb) {
    QJsonObject o{
        {QStringLiteral("access_token"),  t.accessToken},
        {QStringLiteral("refresh_token"), t.refreshToken},
        {QStringLiteral("expires_at"),    static_cast<double>(t.expiresAtUnix)},
        {QStringLiteral("email"),         t.accountEmail},
    };
    auto* job = new WritePasswordJob(serviceName(), this);
    job->setKey(keyName());
    job->setTextData(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    connect(job, &WritePasswordJob::finished, this, [cb = std::move(cb)](QKeychain::Job* j) {
        auto* w = static_cast<WritePasswordJob*>(j);
        cb(w->error() == Error::NoError, w->errorString());
    });
    job->start();
}

void TokenStore::erase(std::function<void(bool, QString)> cb) {
    auto* job = new DeletePasswordJob(serviceName(), this);
    job->setKey(keyName());
    connect(job, &DeletePasswordJob::finished, this, [cb = std::move(cb)](QKeychain::Job* j) {
        auto* d = static_cast<DeletePasswordJob*>(j);
        if (d->error() == Error::NoError || d->error() == Error::EntryNotFound) {
            cb(true, {});
        } else {
            cb(false, d->errorString());
        }
    });
    job->start();
}

}  // namespace fc::auth
