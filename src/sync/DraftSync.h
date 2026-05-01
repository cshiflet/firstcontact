#pragma once

#include <QObject>

namespace fc::api { class GmailClient; }

namespace fc::sync {

// Pushes locally-modified drafts up to Gmail via drafts.create / drafts.update.
// Pulls are handled by SyncService's normal initial/incremental sync paths
// (drafts show up in messages.list for label DRAFT). For v1 we only PUSH —
// remote-side draft edits are reflected on the next message refresh.
class DraftSync : public QObject {
    Q_OBJECT
public:
    DraftSync(fc::api::GmailClient* gmail, QObject* parent = nullptr);

    void start(int intervalMs = 30'000);
    void stop();
    void flush();

signals:
    void draftPushed(const QString& localId, const QString& gmailDraftId);
    void draftFailed(const QString& localId, const QString& reason);

private:
    struct Impl;
    Impl* d_;
};

}  // namespace fc::sync
