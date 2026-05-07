#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace fc::api { class GmailClient; }

namespace fc::sync {

// Drains the local outbox by sending RFC 5322 blobs to Gmail. On each tick,
// pulls items whose next_retry_at <= now, dispatches them serially, and
// updates the outbox row on success/failure.
//
// Multi-account: each cache row carries an account_id. The worker looks
// up the right GmailClient for each row via a resolver supplied at
// construction (Bootstrap binds it to AccountManager::contextFor).
// Rows whose account is unknown / signed-out are skipped — they'll be
// retried once the account context is rebuilt or eventually evicted by
// the account-removal cascade.
class OutboxWorker : public QObject {
    Q_OBJECT
public:
    using GmailResolver = std::function<fc::api::GmailClient*(const QString& accountId)>;

    OutboxWorker(fc::api::GmailClient* gmail, QObject* parent = nullptr);
    OutboxWorker(GmailResolver resolver, QObject* parent = nullptr);

    void start(int intervalMs = 30'000);
    void stop();

    // Triggers a one-shot drain attempt across every account. v1 has a
    // single GmailClient instance; the worker still iterates per-account
    // due rows because each row carries the account id its API path will
    // be charged against once AccountContext lands.
    void flush();

signals:
    void itemSent(qint64 outboxId, const QString& gmailMessageId);
    void itemFailed(qint64 outboxId, const QString& error);

private:
    struct Impl;
    Impl* d_;
};

}  // namespace fc::sync
