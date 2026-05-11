#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace fc::cache {

struct DraftRow {
    QString accountId;
    QString id;                        // local id ("tmp-…") until first sync, then Gmail draftId
    QString gmailDraftId;              // empty until synced
    QString messageId;                 // populated after first sync
    QString threadId;
    QString inReplyToMessageId;
    QString subject;
    QStringList toAddrs;
    QStringList ccAddrs;
    QStringList bccAddrs;
    QString bodyText;
    qint64  updatedAt = 0;
    bool    dirty = false;             // needs push to Gmail
};

class DraftRepository {
public:
    // Per-account API.
    static QString               upsert(const QString& accountId, const DraftRow& d);
    static std::vector<DraftRow> listLocal(const QString& accountId);
    static DraftRow              byId(const QString& accountId, const QString& id);
    static std::vector<DraftRow> dirtyDrafts(const QString& accountId);
    static void                  markSynced(const QString& accountId,
                                            const QString& localId,
                                            const QString& gmailDraftId);
    static void                  remove(const QString& accountId,
                                        const QString& id);

    // Cross-account helper used by DraftSync — pulls dirty drafts across
    // every signed-in account so a single worker iteration can flush them
    // all. Returns rows hydrated with accountId so the worker knows
    // which AccountContext.gmail to dispatch each draft through.
    static std::vector<DraftRow> dirtyDraftsAllAccounts();
};

}  // namespace fc::cache
