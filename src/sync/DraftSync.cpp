#include "DraftSync.h"

#include "api/GmailClient.h"
#include "cache/DraftRepository.h"
#include "util/MimeBuilder.h"

#include <QMetaObject>
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
    DraftSync::GmailResolver resolver;
    QTimer* timer = nullptr;
    bool busy = false;
};

DraftSync::DraftSync(fc::api::GmailClient* gmail, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->gmail = gmail;
}

DraftSync::DraftSync(GmailResolver resolver, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->resolver = std::move(resolver);
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
    auto drafts = fc::cache::DraftRepository::dirtyDraftsAllAccounts();
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
        const QString accountId = draft.accountId;

        fc::api::GmailClient* client = d_->resolver
            ? d_->resolver(accountId)
            : d_->gmail;
        if (!client) {
            qInfo("DraftSync: skipping draft %s (account %s has no active "
                  "GmailClient)", qUtf8Printable(localId),
                  qUtf8Printable(accountId));
            onDone();
            continue;
        }

        if (draft.gmailDraftId.isEmpty()) {
            // client lives on its account's sync thread; bounce the
            // createDraft call onto that thread so QNetworkAccessManager
            // isn't poked from the UI thread. Callback runs on the sync
            // thread (DraftRepository will pick up that thread's
            // per-thread DB connection); the worker's signals reach UI
            // via queued delivery on emit.
            const QString threadId = draft.threadId;
            QMetaObject::invokeMethod(client,
                [client, rfc, threadId, self, localId, accountId, onDone] {
                client->createDraft(rfc, threadId,
                    [self, localId, accountId, onDone]
                    (QString gmailDraftId, fc::api::ApiError err) {
                        if (err) {
                            if (self) emit self->draftFailed(localId, err.message);
                        } else {
                            fc::cache::DraftRepository::markSynced(accountId,
                                                                    localId,
                                                                    gmailDraftId);
                            if (self) emit self->draftPushed(localId, gmailDraftId);
                        }
                        onDone();
                    });
            }, Qt::QueuedConnection);
        } else {
            // Phase 4 will add drafts.update; for now just clear the dirty
            // flag in place — the next full sync will pick up the canonical
            // version from Gmail and we'll push subsequent edits as new.
            // (Avoids duplicate drafts cluttering the user's mailbox.)
            auto row = draft;
            row.dirty = false;
            fc::cache::DraftRepository::upsert(accountId, row);
            emit draftPushed(localId, draft.gmailDraftId);
            onDone();
        }
    }
}

}  // namespace fc::sync
