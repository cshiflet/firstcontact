#pragma once

#include "Errors.h"
#include "models/Message.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace fc::api {

class RestClient;

// High-level façade over the Gmail REST API. Phase 1 surface is intentionally
// small: list message ids in a label, fetch a single message in full. Phase 2/3
// add labels.list, history.list, batch metadata, drafts.*, etc.
class GmailClient : public QObject {
    Q_OBJECT
public:
    explicit GmailClient(RestClient* rest, QObject* parent = nullptr);

    struct ListPage {
        QStringList ids;
        QString     nextPageToken;
        qint64      resultSizeEstimate = 0;
    };

    // GET /gmail/v1/users/me/messages?labelIds=…&q=…&pageToken=…
    void listMessages(const QString& labelId,
                      const QString& q,
                      const QString& pageToken,
                      int maxResults,
                      std::function<void(ListPage, ApiError)> cb);

    // GET /gmail/v1/users/me/messages/{id}?format=<format>
    //
    // `format` is "full" (default) or "metadata". The metadata variant
    // returns headers/labels/snippet/internalDate without bodies — used
    // by the meta-first sync path so we don't pull bodies for every
    // synced message up front.
    void getMessage(const QString& id,
                    const QString& format,
                    std::function<void(fc::Message, ApiError)> cb);

    // Convenience overload kept for callers that just want a full
    // body — pre-batch code paths and on-demand fetch from the reader.
    void getMessage(const QString& id,
                    std::function<void(fc::Message, ApiError)> cb);

    // Issues one Gmail /batch round-trip that fetches up to ~50
    // messages by id. Returns a vector aligned with `ids`; entries
    // whose sub-response was a non-success have an empty Message id
    // and the corresponding ApiError. The top-level error covers a
    // network-level failure of the batch envelope itself (in which
    // case `results` is empty).
    //
    // `format` is "metadata" (default for the meta-first pass) or
    // "full". The caller decides how many ids fit in one batch; Gmail
    // documents a hard limit of 100 sub-requests per batch but
    // practical sweet spots are 20-50 to keep individual response
    // sizes manageable.
    struct BatchMessageResult {
        fc::Message msg;
        ApiError    err;
    };
    void batchGetMessages(const QStringList& ids,
                           const QString& format,
                           std::function<void(std::vector<BatchMessageResult>,
                                                ApiError)> cb);

    // GET /gmail/v1/users/me/messages/{messageId}/attachments/{attachmentId}
    // Returns the raw decoded bytes (the API replies with base64url-encoded
    // data; we decode here so callers just get the binary payload).
    void getAttachment(const QString& messageId,
                       const QString& attachmentId,
                       std::function<void(QByteArray, ApiError)> cb);

    // GET /gmail/v1/users/me/profile
    struct Profile {
        QString emailAddress;
        QString historyId;
    };
    void getProfile(std::function<void(Profile, ApiError)> cb);

    // Labels --------------------------------------------------------------

    struct Label {
        QString id;
        QString name;
        QString type;     // "system" | "user"
        QString colorBg;
        QString colorFg;
    };
    void listLabels(std::function<void(std::vector<Label>, ApiError)> cb);
    void createLabel(const QString& name,
                     std::function<void(Label, ApiError)> cb);
    void updateLabel(const QString& id, const QString& newName,
                     std::function<void(Label, ApiError)> cb);
    void deleteLabel(const QString& id,
                     std::function<void(ApiError)> cb);

    // Modify message labels (add/remove). Used by both online edits and the
    // pending-ops worker after offline edits.
    void modifyMessage(const QString& messageId,
                       const QStringList& addLabels,
                       const QStringList& removeLabels,
                       std::function<void(ApiError)> cb);

    // History -------------------------------------------------------------

    struct HistoryEntry {
        QString id;
        QStringList messagesAdded;
        QStringList messagesDeleted;
        struct LabelDelta {
            QString messageId;
            QStringList labelIds;
        };
        std::vector<LabelDelta> labelsAdded;
        std::vector<LabelDelta> labelsRemoved;
    };
    struct HistoryPage {
        std::vector<HistoryEntry> entries;
        QString historyId;
        QString nextPageToken;
        bool    historyTooOld = false;   // 404
    };
    void listHistory(const QString& startHistoryId,
                     const QString& pageToken,
                     std::function<void(HistoryPage, ApiError)> cb);

    // Send / drafts -------------------------------------------------------

    // Sends a fully-built RFC 5322 blob. `threadId` ties the message into an
    // existing thread when replying.
    void sendRaw(const QByteArray& rfc5322,
                 const QString& threadId,
                 std::function<void(QString messageId, ApiError)> cb);

    // Phase-3 draft helpers — full surface lands when ComposeWindow is wired.
    void createDraft(const QByteArray& rfc5322,
                     const QString& threadId,
                     std::function<void(QString draftId, ApiError)> cb);
    void deleteDraft(const QString& draftId,
                     std::function<void(ApiError)> cb);

private:
    RestClient* rest_;
};

}  // namespace fc::api
