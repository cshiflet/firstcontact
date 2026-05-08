#include "DraftSync.h"

#include "api/GmailClient.h"
#include "cache/DraftRepository.h"
#include "util/MimeBuilder.h"

#include <QPointer>
#include <QTimer>

namespace fc::sync {

namespace {

QByteArray buildRfc(const fc::cache::DraftRow& d) {
    fc::util::OutgoingMessage m;
    m.to                 = d.toAddrs;
    m.cc                 = d.ccAddrs;
    m.bcc                = d.bccAddrs;
    m.subject            = d.subject;
    m.bodyText           = d.bodyText;
    m.rfc822InReplyTo    = d.inReplyToMessageId.isEmpty()
                              ? QString()
                              : QStringLiteral("<%1@gmail>").arg(d.inReplyToMessageId);
    return fc::util::MimeBuilder::build(m);
}

}  // namespace

struct DraftSync::Impl {
    fc::api::GmailClient* gmail = nullptr;
    QTimer* timer = nullptr;
    bool busy = false;
};

DraftSync::DraftSync(fc::api::GmailClient* gmail, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->gmail = gmail;
}

void DraftSync::start(int intervalMs) {
    if (!d_->timer) {
        d_->timer = new QTimer(this);
        connect(d_->timer, &QTimer::timeout, this, &DraftSync::flush);
    }
    d_->timer->start(intervalMs);
}

void DraftSync::stop() { if (d_->timer) d_->timer->stop(); }

void DraftSync::flush() {
    if (d_->busy) return;
    auto drafts = fc::cache::DraftRepository::dirtyDrafts();
    if (drafts.empty()) return;
    d_->busy = true;

    // Single QPointer guard reused across all in-flight callbacks
    // for this flush. onDone previously captured raw `this`, which
    // would UAF if the worker was destroyed between dispatch and
    // every callback resolving (typical at sign-out or shutdown).
    QPointer<DraftSync> self(this);
    auto remaining = std::make_shared<int>(int(drafts.size()));
    auto onDone = [self, remaining]() {
        if (--(*remaining) > 0) return;
        if (self) self->d_->busy = false;
    };

    for (const auto& draft : drafts) {
        const QByteArray rfc = buildRfc(draft);
        const QString localId = draft.id;

        if (draft.gmailDraftId.isEmpty()) {
            d_->gmail->createDraft(rfc, draft.threadId,
                [self, localId, onDone](QString gmailDraftId, fc::api::ApiError err) {
                    if (err) {
                        if (self) emit self->draftFailed(localId, err.message);
                    } else {
                        fc::cache::DraftRepository::markSynced(localId, gmailDraftId);
                        if (self) emit self->draftPushed(localId, gmailDraftId);
                    }
                    onDone();
                });
        } else {
            // Phase 4 will add drafts.update; for now just clear the dirty
            // flag in place — the next full sync will pick up the canonical
            // version from Gmail and we'll push subsequent edits as new.
            // (Avoids duplicate drafts cluttering the user's mailbox.)
            auto row = draft;
            row.dirty = false;
            fc::cache::DraftRepository::upsert(row);
            emit draftPushed(localId, draft.gmailDraftId);
            onDone();
        }
    }
}

}  // namespace fc::sync
