#include "OutboxWorker.h"

#include "api/GmailClient.h"
#include "cache/OutboxRepository.h"

#include <QDateTime>
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
    fc::api::GmailClient* gmail = nullptr;
    QTimer* timer = nullptr;
    bool busy = false;
};

OutboxWorker::OutboxWorker(fc::api::GmailClient* gmail, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->gmail = gmail;
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
    auto items = fc::cache::OutboxRepository::dueForSend();
    if (items.empty()) return;
    d_->busy = true;

    auto remaining = std::make_shared<int>(int(items.size()));
    auto onDone = [this, remaining]() {
        if (--(*remaining) > 0) return;
        d_->busy = false;
    };

    for (const auto& it : items) {
        fc::cache::OutboxRepository::markSending(it.id);
        d_->gmail->sendRaw(it.rfc5322, it.threadId,
            [this, id = it.id, attempt = it.attemptCount + 1, onDone]
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
                    emit itemFailed(id, err.message);
                } else {
                    fc::cache::OutboxRepository::markSent(id);
                    emit itemSent(id, gmailMsgId);
                }
                onDone();
            });
    }
}

}  // namespace fc::sync
