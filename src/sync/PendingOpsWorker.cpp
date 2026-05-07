#include "PendingOpsWorker.h"

#include "api/GmailClient.h"
#include "cache/PendingOpsRepository.h"
#include "util/DryRun.h"

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

    auto remaining = std::make_shared<int>(int(ops.size()));
    auto onDone = [this, remaining]() {
        if (--(*remaining) > 0) return;
        d_->busy = false;
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

        QPointer<PendingOpsWorker> self(this);
        client->modifyMessage(op.messageId, op.addLabels, op.removeLabels,
            [self, id = op.id, attempts = op.attempts, onDone]
            (fc::api::ApiError err) {
                if (!err) {
                    fc::cache::PendingOpsRepository::remove(id);
                    if (self) emit self->itemReconciled(id);
                } else if (err.kind == fc::api::ApiErrorKind::BadRequest ||
                           err.kind == fc::api::ApiErrorKind::NotFound) {
                    fc::cache::PendingOpsRepository::remove(id);
                    if (self) emit self->itemDropped(id, err.message);
                } else if (attempts + 1 >= kGiveUpAttempts) {
                    fc::cache::PendingOpsRepository::remove(id);
                    if (self) emit self->itemDropped(id, err.message);
                } else {
                    fc::cache::PendingOpsRepository::markAttempt(id, err.message);
                }
                onDone();
            });
    }
}

}  // namespace fc::sync
