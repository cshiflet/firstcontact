#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace fc {

struct Attachment {
    QString id;          // Gmail attachmentId
    QString filename;
    QString mimeType;
    int     size = 0;
    QString localPath;   // populated once downloaded
};

struct Message {
    QString id;
    // The account this message belongs to. Cache row's account_id, set
    // by every repository read. Outgoing upserts must populate this so
    // the row lands under the right account; SyncService stamps it from
    // its AccountContext, MainWindow stamps it from the active account
    // when composing locally.
    QString accountId;
    QString threadId;
    QString historyId;
    qint64  internalDate = 0;     // ms since epoch
    int     sizeEstimate = 0;

    QString fromName;
    QString fromAddr;
    QString replyTo;
    QStringList toAddrs;
    QStringList ccAddrs;
    QStringList bccAddrs;

    QString subject;
    QString snippet;
    QString bodyText;             // text/plain part (may be empty)
    QString bodyHtml;             // text/html part (may be empty)
    bool    bodyHtmlPresent = false;

    bool isUnread    = false;
    bool isStarred   = false;
    bool isImportant = false;
    bool hasAttachment = false;

    // Tracks whether the cached row has the FULL body or just metadata.
    // Populated by rowToMessage from the messages.fetched_format column;
    // stays empty for freshly-parsed wire responses (where the body
    // presence on the Message itself drives downstream decisions).
    // The ReaderPane and on-demand fetch path key off this to decide
    // whether to issue a follow-up GET with format=full when the user
    // opens a message that was only meta-synced.
    QString fetchedFormat;

    // 0 = plaintext, 1 = zstd_dict_v1, 2 = orphan (bytes compressed
    // against a dict we no longer have; reader triggers re-fetch).
    // Mirrors messages.body_compression.
    int     bodyCompression = 0;

    QStringList labelIds;
    std::vector<Attachment> attachments;

    // Thread aggregate. Populated only by listThreadsByLabel /
    // searchFtsThreads — those queries return one row per thread, hydrated
    // with the LATEST message's per-message fields (id / subject / from /
    // snippet / internalDate) plus these whole-thread roll-ups. For all
    // other paths (byId / listByLabel / byThread / searchFts) threadCount
    // stays 0 and the OR'd flags stay false.
    int  threadCount   = 0;     // 0 = single-message context, ignore aggregates
    bool threadHasUnread     = false;
    bool threadHasStarred    = false;
    bool threadHasAttachment = false;

    // UI-side flag set by MessageListModel when a thread is expanded
    // inline. The model injects child rows after the parent thread
    // row; the delegate uses this to draw indented / dimmer chrome
    // and hide the count badge / chevron on those rows. NEVER comes
    // back from the cache repos — purely a transient view-state bit.
    bool isThreadChild = false;
};

}  // namespace fc
