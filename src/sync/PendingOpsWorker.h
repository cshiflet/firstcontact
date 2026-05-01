#pragma once

#include <QObject>

namespace fc::api { class GmailClient; }

namespace fc::sync {

// Drains pending_ops by replaying each cached label-modify against the Gmail
// API. Optimistic local edits land in the cache immediately; this worker
// reconciles them server-side as the network allows. On permanent 4xx errors
// the row is removed (giving up) — same approach as OutboxWorker.
class PendingOpsWorker : public QObject {
    Q_OBJECT
public:
    PendingOpsWorker(fc::api::GmailClient* gmail, QObject* parent = nullptr);

    void start(int intervalMs = 30'000);
    void stop();
    void flush();

signals:
    void itemReconciled(qint64 opId);
    void itemDropped(qint64 opId, const QString& reason);

private:
    struct Impl;
    Impl* d_;
};

}  // namespace fc::sync
