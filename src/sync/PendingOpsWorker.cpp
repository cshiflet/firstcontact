#include "PendingOpsWorker.h"

#include "api/GmailClient.h"
#include "cache/PendingOpsRepository.h"
#include "util/DryRun.h"

#include <QMetaObject>
#include <QPointer>
#include <QTimer>

namespace fc::sync {

namespace {
constexpr int kGiveUpAttempts = 8;   // ~ matches outbox max with backoff
}

struct PendingOpsWorker::Impl {
    fc::api::GmailClient* gmail = nullptr;
    PendingOpsWorker::GmailResolver resolver;
    QTimer* timer = nullptr;
    bool busy = false;
};

PendingOpsWorker::PendingOpsWorker(fc::api::GmailClient* gmail, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->gmail = gmail;
}

PendingOpsWorker::PendingOpsWorker(GmailResolver resolver, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->resolver = std::move(resolver);
}

void PendingOpsWorker::start(int intervalMs) {
    if (!d_->timer) {
        d_->timer = new QTimer(this);
        connect(d_->timer, &QTimer::timeout, this, &PendingOpsWorker::flush);
    }
    d_->timer->start(intervalMs);
}

void PendingOpsWorker::stop() { if (d_->timer) d_->timer->stop(); }

void PendingOpsWorker::flush() {
    if (d_->busy) return;
    // Hard stop in dry-run: leave the queue in place so it drains
    // automatically the next time the user runs without FC_DRY_RUN.
    // Without this gate, an op enqueued before the flag was set could
    // still hit Gmail on the next scheduler tick.
    if (fc::util::DryRun::block(QStringLiteral("pending-ops-flush"))) return;
    auto ops = fc::cache::PendingOpsRepository::dueAllAccounts();
    if (ops.empty()) return;
    d_->busy = true;

    // Single QPointer reused by every callback for this flush. The
    // inner modifyMessage callbacks already used `self`, but the
    // shared onDone captured raw `this` and would UAF on the d_->busy
    // dereference if the worker died while callbacks were still in
    // flight (sign-out, shutdown).
    QPointer<PendingOpsWorker> self(this);
    auto remaining = std::make_shared<int>(int(ops.size()));
    auto onDone = [self, remaining]() {
        if (--(*remaining) > 0) return;
        if (self) self->d_->busy = false;
    };

    for (const auto& op : ops) {
        if (op.opType != QLatin1String("modify")) {
            // Unknown op type — drop, would block the queue forever otherwise.
            fc::cache::PendingOpsRepository::remove(op.id);
            emit itemDropped(op.id, QStringLiteral("unknown opType"));
            onDone();
            continue;
        }

        fc::api::GmailClient* client = d_->resolver
            ? d_->resolver(op.accountId)
            : d_->gmail;
        if (!client) {
            qInfo("PendingOpsWorker: skipping op %lld (account %s has no "
                  "active GmailClient)",
                  op.id, qUtf8Printable(op.accountId));
            onDone();
            continue;
        }

        // client lives on its AccountContext's sync thread; bounce the
        // modify call onto that thread so QNetworkAccessManager isn't
        // touched cross-thread. The callback runs on the sync thread
        // and uses that thread's per-thread DB connection for the
        // PendingOpsRepository writes; the worker's signals re-cross
        // back to UI via queued delivery on emit.
        const QString messageId   = op.messageId;
        const QStringList addList = op.addLabels;
        const QStringList remList = op.removeLabels;
        const qint64      opId    = op.id;
        const int         attempts = op.attempts;
        QMetaObject::invokeMethod(client,
            [client, messageId, addList, remList, opId, attempts, self, onDone] {
            client->modifyMessage(messageId, addList, remList,
                [self, opId, attempts, onDone]
                (fc::api::ApiError err) {
                    if (!err) {
                        fc::cache::PendingOpsRepository::remove(opId);
                        if (self) emit self->itemReconciled(opId);
                    } else if (err.kind == fc::api::ApiErrorKind::BadRequest ||
                               err.kind == fc::api::ApiErrorKind::NotFound) {
                        fc::cache::PendingOpsRepository::remove(opId);
                        if (self) emit self->itemDropped(opId, err.message);
                    } else if (attempts + 1 >= kGiveUpAttempts) {
                        fc::cache::PendingOpsRepository::remove(opId);
                        if (self) emit self->itemDropped(opId, err.message);
                    } else {
                        fc::cache::PendingOpsRepository::markAttempt(opId, err.message);
                    }
                    onDone();
                });
        }, Qt::QueuedConnection);
    }
}

}  // namespace fc::sync
