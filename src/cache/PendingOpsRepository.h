#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace fc::cache {

struct PendingOp {
    qint64      id = 0;
    QString     opType;            // "modify"
    QString     messageId;
    QStringList addLabels;
    QStringList removeLabels;
    qint64      createdAt = 0;
    int         attempts = 0;
    QString     lastError;
};

class PendingOpsRepository {
public:
    static qint64 enqueueModify(const QString& messageId,
                                const QStringList& addLabels,
                                const QStringList& removeLabels);

    static std::vector<PendingOp> due();

    static void markAttempt(qint64 id, const QString& err);
    static void remove(qint64 id);
};

}  // namespace fc::cache
