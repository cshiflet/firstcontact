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
    // Replace all attachment rows for the given message. Called inside the
    // same transaction as the message upsert (the FK to messages(id)
    // requires the message row to exist first).
    static void replaceForMessage(const QString& messageId,
                                  const std::vector<fc::Attachment>& attachments);

    static std::vector<fc::Attachment> byMessage(const QString& messageId);

    // Updates local_path once an attachment has been downloaded.
    static void markDownloaded(const QString& attachmentId,
                               const QString& localPath);
};

}  // namespace fc::cache
