#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace fc::cache {

struct OutboxItem {
    qint64      id = 0;
    QString     accountId;
    QString     state;          // "queued" | "sending" | "sent" | "failed"
    QByteArray  rfc5322;
    QString     threadId;
    QString     inReplyToMessageId;
    qint64      createdAt = 0;
    int         attemptCount = 0;
    qint64      nextRetryAt = 0;
    qint64      sendAt = 0;     // 0 = send immediately; otherwise ms-epoch
                                // wall-clock at which the row becomes due
                                // (scheduled send).
    QString     lastError;
};

class OutboxRepository {
public:
    // Per-account API.
    static qint64                  enqueue(const QString& accountId,
                                           const OutboxItem& item);
    static std::vector<OutboxItem> dueForSend(const QString& accountId);

    // markSending/markSent/markFailed key off the autoincrement row id
    // (globally unique even with multi-account), so they don't need an
    // accountId parameter.
    static void markSending(qint64 id);
    static void markSent(qint64 id);
    static void markFailed(qint64 id, const QString& err, qint64 nextRetryAt);

    // Cross-account helper for OutboxWorker — pulls due rows across
    // every account so a single tick can drain them all.
    static std::vector<OutboxItem> dueForSendAllAccounts();
};

}  // namespace fc::cache
