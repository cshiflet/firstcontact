#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace fc::cache {

struct PendingOp {
    qint64      id = 0;
    QString     accountId;
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
    // Per-account API.
    static qint64                 enqueueModify(const QString& accountId,
                                                 const QString& messageId,
                                                 const QStringList& addLabels,
                                                 const QStringList& removeLabels);
    static std::vector<PendingOp> due(const QString& accountId);

    // Cross-account variant for PendingOpsWorker — drains every account's
    // backlog from a single tick.
    static std::vector<PendingOp> dueAllAccounts();

    // markAttempt/remove key off the autoincrement row id.
    static void markAttempt(qint64 id, const QString& err);
    static void remove(qint64 id);

    // Legacy zero-arg overloads.
    static qint64                 enqueueModify(const QString& messageId,
                                                 const QStringList& addLabels,
                                                 const QStringList& removeLabels);
    static std::vector<PendingOp> due();
};

}  // namespace fc::cache
