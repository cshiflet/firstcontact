#pragma once

#include "api/Errors.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace fc::api { class GmailClient; }

namespace fc::sync {

// Coordinates initial and incremental sync against Gmail, persisting results
// through fc::cache repositories. Per-account SyncService instances live on
// the UI thread with their AccountContext (see AccountContext.h). The legacy
// anonymous SyncService owned by Bootstrap also stays on the UI thread and
// never sees any real work — runOnce short-circuits on the empty accountId.
class SyncService : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, InitialSync, IncrementalSync, TopUp };

    SyncService(fc::api::GmailClient* gmail, QObject* parent = nullptr);

    // Switches the per-account context the service operates on. Until
    // AccountContext lands in step 4, MainWindow calls this whenever the
    // active account changes; SyncService scopes every cache write to
    // this id and reads/writes its history_id from account_meta scoped
    // to it. The accountId may be the empty string before sign-in, in
    // which case runOnce() short-circuits.
    void setAccountId(const QString& accountId);
    QString accountId() const;

    State state() const;
    QString lastError() const;

    // Triggers an initial sync if no historyId is recorded, otherwise an
    // incremental delta. Idempotent — concurrent calls are coalesced.
    void runOnce();

    // Starts a periodic timer (60s default). Call on the main thread.
    void startScheduler(int intervalMs = 60'000);
    void stopScheduler();

    // Pulls one server-side page of message ids for the given label,
    // dedupes against the cache, and fetches full bodies for any
    // entries we don't already have. Used to fill cache gaps for
    // labels that weren't part of the initial-sync seed set (every
    // user label, plus any system label outside INBOX / SENT / DRAFT
    // / STARRED).
    //
    // Pagination state lives in the meta table — successive calls
    // walk older pages until the label is exhausted, at which point
    // the saved token resets and the cycle starts over from the most
    // recent message. Skips the round-trip entirely for labels that
    // were already in the initial-sync seed set, since incremental
    // sync keeps them up to date.
    void topUpLabel(const QString& labelId);

    // Walks the label end-to-end: chains topUpLabel pages until the
    // server returns no nextPageToken. Honors a user-controlled
    // cancel flag (cancelCacheLabel) so the workflow can be aborted
    // mid-flight. Emits cacheLabelProgress / cacheLabelFinished so
    // MainWindow can drive a status-bar readout.
    void cacheLabelComplete(const QString& labelId);
    void cancelCacheLabel();

    // Fetches one message body in full (`format=full`) and upserts the
    // row, then fires `cb` when complete. Used by the reader pane's
    // on-demand body load: when low-bandwidth mode is on and a user
    // opens a message whose cache row only carries metadata, this
    // pulls the body asynchronously. cb runs on this object's thread, which is
    // currently the UI thread for AccountContext-owned services.
    void fetchBodyOnDemand(const QString& messageId,
                            std::function<void(fc::api::ApiError)> cb);

    // One tick of the opt-in background crawler. Picks an
    // un-exhausted label and runs ONE topUpLabel page for it; bails
    // when the user has signalled a foreground top-up is in flight.
    // The MainWindow / Preferences layer drives a QTimer that calls
    // this on a configurable interval.
    void tickBackgroundCrawl();

    // Starts / stops the per-service background-crawl timer. Re-
    // applying with the same values is a no-op restart so the
    // Settings dialog can call configure() unconditionally after
    // any change. intervalSec is clamped to >= 1.
    void configureBackgroundCrawl(bool enabled, int intervalSec);

    // Drops the per-label crawl-exhausted flags so the next
    // tickBackgroundCrawl revisits labels that have already been
    // walked end-to-end. Used by the Settings "Reset crawl
    // progress" button.
    void resetBackgroundCrawlProgress();

signals:
    void stateChanged(fc::sync::SyncService::State s);
    void labelsUpdated();
    void messagesUpdated();
    void newMessages(int count);
    void failed(const QString& reason);
    // Fires after every successful getProfile so the auth layer can
    // store the signed-in account's email (it isn't returned in the
    // OAuth token response). Wired up in MainWindow.
    void profileFetched(const QString& email);

    // Per-label top-up bracket: emitted around topUpLabel so the UI
    // can show a label-specific "Syncing <name>…" / "<name>: Done"
    // status instead of the generic stateChanged message. Fire-and-
    // forget — listening is optional. `serverExhausted` tells the
    // caller whether the server returned an empty nextPageToken,
    // i.e., we've walked the label end-to-end.
    void topUpStarted(const QString& labelId);
    void topUpFinished(const QString& labelId, int newRowsStored,
                        bool serverExhausted);

    // Fires once per account when the cached-body count first crosses
    // the dictionary-training threshold AND the user hasn't yet been
    // asked whether to compress. MainWindow listens, shows the
    // "Compress DB / Disable compression" dialog. SyncService doesn't
    // act on the answer itself — that's the UI's call.
    void compressionPromptDue(int bodyCount);

    // Lifecycle of a cacheLabelComplete run. progress fires after
    // every successful top-up page with the running total of stored
    // rows; finished fires when the chain settles (serverExhausted
    // OR cancel). Total in finished is the number of NEW rows the
    // entire walk added (already-cached pages are walked through
    // but don't contribute).
    void cacheLabelProgress(const QString& labelId, int totalStored);
    void cacheLabelFinished(const QString& labelId, int totalStored,
                              bool cancelled);

private:
    void doInitialSync();
    void doIncrementalSync();
    // `bodyFormat` is "full" (default) or "metadata". The metadata
    // path issues a single Gmail /batch round-trip via
    // GmailClient::batchGetMessages instead of N concurrent
    // getMessage calls — N+1 → 1 HTTP request per page when low-
    // bandwidth mode is on. Both paths funnel through the same
    // MessageRepository::upsertMany commit, so storage looks
    // identical except for the bodies being absent for metadata
    // rows.
    void fetchAndStoreMessages(const QStringList& ids,
                               int newCount,
                               bool isInitial,
                               std::function<void()> done = {},
                               const QString& bodyFormat = {});
    void setState(State s);
    // Internal step-walker for topUpLabel. Each invocation pulls one
    // server page; when every id on the page is already cached we
    // advance the saved cursor and re-enter (bounded by `pagesWalked`)
    // so the user-visible top-up actually surfaces new rows instead
    // of stalling once the deduplication empties out the page.
    void topUpLabelStep(const QString& labelId, int pagesWalked,
                         int totalStoredSoFar);

    struct Impl;
    Impl* d_;
};

}  // namespace fc::sync
