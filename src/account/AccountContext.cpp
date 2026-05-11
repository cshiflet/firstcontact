#include "AccountContext.h"

#include "api/GmailClient.h"
#include "api/RestClient.h"
#include "auth/ClientConfig.h"
#include "auth/OAuthClient.h"
#include "auth/TokenStore.h"
#include "cache/Database.h"
#include "sync/SyncService.h"

#include <QMetaObject>
#include <QThread>

namespace fc::account {

AccountContext::AccountContext(const QString& accountId,
                                fc::auth::ClientConfig* config,
                                fc::auth::TokenStore*   tokenStore,
                                QObject* parent)
    : QObject(parent), accountId_(accountId) {
    // Each context owns its full per-account stack PLUS the QThread
    // those QObjects live on. The stack is constructed parentless so
    // moveToThread doesn't trip Qt's "cannot move object with parent"
    // guard; we keep raw pointers in members instead and manage the
    // lifetime explicitly in the destructor.
    syncThread_ = new QThread();
    syncThread_->setObjectName(QStringLiteral("fc-sync-") + accountId);

    auth_  = new fc::auth::OAuthClient(config, tokenStore, accountId);
    rest_  = new fc::api::RestClient(auth_);
    gmail_ = new fc::api::GmailClient(rest_);
    sync_  = new fc::sync::SyncService(gmail_);

    auth_->moveToThread(syncThread_);
    rest_->moveToThread(syncThread_);
    gmail_->moveToThread(syncThread_);
    sync_->moveToThread(syncThread_);

    // SQLite connections are per-thread (see Database::perThreadName);
    // initialise on the sync thread the first time it spins up so any
    // repository call from a SyncService callback has a live handle.
    // The migration runner is idempotent — it only re-applies if the
    // schema version differs, which it can't for an already-migrated
    // DB.
    QObject::connect(syncThread_, &QThread::started, sync_, [] {
        fc::cache::Database::initialize();
    });

    syncThread_->start();

    qInfo("AccountContext: built ctx=%p sync=%p for accountId='%s' "
          "on thread '%s'",
          static_cast<void*>(this), static_cast<void*>(sync_),
          qUtf8Printable(accountId),
          qUtf8Printable(syncThread_->objectName()));

    // setAccountId is a normal method on SyncService, not a thread-safe
    // accessor — invoke it on the sync thread to avoid touching d_->
    // members from the UI thread. Queued (not blocking) so we don't
    // race with the sync thread's event loop coming up; subsequent
    // calls into SyncService (runOnce, topUpLabel, etc.) also go
    // through invokeMethod and queue after this one, so they'll always
    // see the right accountId when they run.
    QMetaObject::invokeMethod(sync_, [s = sync_, accountId] {
        s->setAccountId(accountId);
    }, Qt::QueuedConnection);
}

AccountContext::~AccountContext() {
    // The four QObjects live on syncThread_; deleting them from the
    // UI thread directly would either trigger Qt's cross-thread-delete
    // warning or destroy QNetworkAccessManager / QSqlDatabase state on
    // the wrong thread. Queue the teardown onto the sync thread (which
    // is still running its event loop at this point), then ask the
    // thread to quit and join it. The local pointers shield us from
    // the `delete this->member_` member-aliasing problem if the
    // destructor were to run concurrently with anything else.
    if (syncThread_) {
        auto* s = sync_;     sync_  = nullptr;
        auto* g = gmail_;    gmail_ = nullptr;
        auto* r = rest_;     rest_  = nullptr;
        auto* a = auth_;     auth_  = nullptr;
        if (syncThread_->isRunning() && s) {
            // s lives on syncThread_, so invokeMethod with target=s
            // dispatches the lambda onto that thread. The deletion has
            // to happen in a fixed order — outer objects first — because
            // SyncService's d_->gmail dangles otherwise; same chain
            // through to OAuthClient.
            QMetaObject::invokeMethod(s, [s, g, r, a] {
                delete s;
                delete g;
                delete r;
                delete a;
            }, Qt::BlockingQueuedConnection);
            syncThread_->quit();
            syncThread_->wait();
        } else {
            // Thread never started or already exited — fall back to a
            // direct delete on whatever thread we're on.
            delete s;
            delete g;
            delete r;
            delete a;
        }
        delete syncThread_;
        syncThread_ = nullptr;
    }
}

}  // namespace fc::account
