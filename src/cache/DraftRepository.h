#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace fc::cache {

struct DraftRow {
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
    static QString upsert(const DraftRow& d);  // returns the row's id
    static std::vector<DraftRow> listLocal();
    static DraftRow byId(const QString& id);
    static std::vector<DraftRow> dirtyDrafts();
    static void   markSynced(const QString& localId, const QString& gmailDraftId);
    static void   remove(const QString& id);
};

}  // namespace fc::cache
