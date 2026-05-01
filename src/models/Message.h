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

    QStringList labelIds;
    std::vector<Attachment> attachments;
};

}  // namespace fc
