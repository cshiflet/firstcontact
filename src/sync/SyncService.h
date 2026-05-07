#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace fc::api { class GmailClient; }

namespace fc::sync {

// Coordinates initial and incremental sync against Gmail, persisting results
// through fc::cache repositories. Phase 2 lives on the main (UI) thread for
// simplicity — sqlite is fast enough that the small inserts don't block the
// UI in noticeable ways. A Phase 4 polish moves this onto its own QThread.
class SyncService : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, InitialSync, IncrementalSync };

    SyncService(fc::api::GmailClient* gmail, QObject* parent = nullptr);

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

private:
    void doInitialSync();
    void doIncrementalSync();
    void fetchAndStoreMessages(const QStringList& ids,
                               int newCount,
                               bool isInitial);
    void setState(State s);

    struct Impl;
    Impl* d_;
};

}  // namespace fc::sync
