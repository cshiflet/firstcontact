#pragma once

#include <QObject>

namespace fc::api { class GmailClient; }

namespace fc::sync {

// Drains the local outbox by sending RFC 5322 blobs to Gmail. On each tick,
// pulls items whose next_retry_at <= now, dispatches them serially, and
// updates the outbox row on success/failure.
class OutboxWorker : public QObject {
    Q_OBJECT
public:
    OutboxWorker(fc::api::GmailClient* gmail, QObject* parent = nullptr);

    void start(int intervalMs = 30'000);
    void stop();

    // Triggers a one-shot drain attempt.
    void flush();

signals:
    void itemSent(qint64 outboxId, const QString& gmailMessageId);
    void itemFailed(qint64 outboxId, const QString& error);

private:
    struct Impl;
    Impl* d_;
};

}  // namespace fc::sync
