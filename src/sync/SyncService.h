#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace fc::api { class GmailClient; }

namespace fc::sync {

// Coordinates initial and incremental sync against Gmail, persisting results
// through fc::cache repositories. Per-account SyncService instances live on
// the AccountContext's dedicated sync thread (see AccountContext.h); SQL
// batch upserts during initial sync and Gmail REST chatter no longer block
// the UI's event loop. The legacy anonymous SyncService owned by Bootstrap
// stays on the main thread (it never sees any real work — runOnce short-
// circuits on the empty accountId).
class SyncService : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, InitialSync, IncrementalSync };

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

private:
    void doInitialSync();
    void doIncrementalSync();
    void fetchAndStoreMessages(const QStringList& ids,
                               int newCount,
                               bool isInitial,
                               std::function<void()> done = {});
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
