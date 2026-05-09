#include "OutboxWorker.h"

#include "api/GmailClient.h"
#include "cache/OutboxRepository.h"

#include <QDateTime>
#include <QPointer>
#include <QTimer>

namespace fc::sync {

namespace {

constexpr int kMaxAttempts = 6;

qint64 backoffNextRetry(int attempt) {
    qint64 delayMs = 30'000;     // 30s base
    for (int i = 1; i < attempt; ++i) delayMs *= 2;
    if (delayMs > 30 * 60'000) delayMs = 30 * 60'000;  // 30m cap
    return QDateTime::currentMSecsSinceEpoch() + delayMs;
}

}  // namespace

struct OutboxWorker::Impl {
    fc::api::GmailClient* gmail = nullptr;   // legacy fallback
    OutboxWorker::GmailResolver resolver;
    QTimer* timer = nullptr;
    bool busy = false;
};

OutboxWorker::OutboxWorker(fc::api::GmailClient* gmail, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->gmail = gmail;
}

OutboxWorker::OutboxWorker(GmailResolver resolver, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->resolver = std::move(resolver);
}

void OutboxWorker::start(int intervalMs) {
    if (!d_->timer) {
        d_->timer = new QTimer(this);
        connect(d_->timer, &QTimer::timeout, this, &OutboxWorker::flush);
    }
    d_->timer->start(intervalMs);
}

void OutboxWorker::stop() { if (d_->timer) d_->timer->stop(); }

void OutboxWorker::flush() {
    if (d_->busy) return;
    // Pull due rows across every account; each row carries the
    // account_id it was enqueued against. Step 6 wires AccountContext
    // and turns this into a per-account GmailClient dispatch.
    auto items = fc::cache::OutboxRepository::dueForSendAllAccounts();
    if (items.empty()) return;
    d_->busy = true;

    // QPointer guards every async callback so a worker destroyed
    // mid-flight (shutdown, sign-out) doesn't dereference a dead
    // d_->busy / signal target. Without the guard, a sendRaw that
    // resolves after teardown would UAF.
    QPointer<OutboxWorker> self(this);
    auto remaining = std::make_shared<int>(int(items.size()));
    auto onDone = [self, remaining]() {
        if (--(*remaining) > 0) return;
        if (self) self->d_->busy = false;
    };

    for (const auto& it : items) {
        fc::api::GmailClient* client = d_->resolver
            ? d_->resolver(it.accountId)
            : d_->gmail;
        if (!client) {
            // Account context isn't available — the account may be
            // signed out or temporarily missing. Skip this row; the
            // next tick (after sign-in / context build) picks it up.
            qInfo("OutboxWorker: skipping row %lld (account %s has no "
                  "active GmailClient)",
                  it.id, qUtf8Printable(it.accountId));
            onDone();
            continue;
        }
        fc::cache::OutboxRepository::markSending(it.id);
        client->sendRaw(it.rfc5322, it.threadId,
            [self, id = it.id, attempt = it.attemptCount + 1, onDone]
            (QString gmailMsgId, fc::api::ApiError err) {
                if (err) {
                    if (attempt >= kMaxAttempts) {
                        fc::cache::OutboxRepository::markFailed(
                            id, err.message,
                            QDateTime::currentMSecsSinceEpoch() + 24 * 60 * 60'000LL);
                    } else {
                        fc::cache::OutboxRepository::markFailed(
                            id, err.message, backoffNextRetry(attempt));
                    }
                    if (self) emit self->itemFailed(id, err.message);
                } else {
                    fc::cache::OutboxRepository::markSent(id);
                    if (self) emit self->itemSent(id, gmailMsgId);
                }
                onDone();
            });
    }
}

}  // namespace fc::sync
