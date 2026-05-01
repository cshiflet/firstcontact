#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace fc::cache {

struct OutboxItem {
    qint64      id = 0;
    QString     state;          // "queued" | "sending" | "sent" | "failed"
    QByteArray  rfc5322;
    QString     threadId;
    QString     inReplyToMessageId;
    qint64      createdAt = 0;
    int         attemptCount = 0;
    qint64      nextRetryAt = 0;
    QString     lastError;
};

class OutboxRepository {
public:
    static qint64 enqueue(const OutboxItem& item);
    static std::vector<OutboxItem> dueForSend();
    static void markSending(qint64 id);
    static void markSent(qint64 id);
    static void markFailed(qint64 id, const QString& err, qint64 nextRetryAt);
};

}  // namespace fc::cache
