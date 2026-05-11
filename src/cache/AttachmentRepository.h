#pragma once

#include "models/Message.h"

#include <QString>

#include <vector>

namespace fc::cache {

// Persists per-message attachment metadata (filename, mime, size, Gmail
// attachmentId, optional local path once downloaded) in the `attachments`
// table introduced in schema 0001_init.sql.
//
// Attachment binary content is NOT stored here — Gmail returns multi-MB
// blobs and we'd be duplicating the user's mailbox storage. We only keep
// the metadata so the reader can list them; the actual bytes get fetched
// on demand via GmailClient::getAttachment when the user clicks Download,
// then optionally cached at the local_path.
class AttachmentRepository {
public:
    static void replaceForMessage(const QString& accountId,
                                  const QString& messageId,
                                  const std::vector<fc::Attachment>& attachments);
    static std::vector<fc::Attachment> byMessage(const QString& accountId,
                                                  const QString& messageId);
    static void markDownloaded(const QString& accountId,
                               const QString& attachmentId,
                               const QString& localPath);
};

}  // namespace fc::cache
