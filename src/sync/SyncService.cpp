#include "SyncService.h"

#include "api/GmailClient.h"
#include "cache/Database.h"
#include "cache/LabelRepository.h"
#include "cache/MessageRepository.h"
#include "cache/MetaRepository.h"

#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

namespace fc::sync {

namespace {

constexpr int kInitialPerLabelCap = 200;       // first-run pages cap
constexpr int kSerialGetBatch     = 25;        // concurrent in-flight gets

// Folders to seed on first sync.
const QStringList& seedLabels() {
    static const QStringList ls{
        QStringLiteral("INBOX"),
        QStringLiteral("SENT"),
        QStringLiteral("DRAFT"),
        QStringLiteral("STARRED"),
    };
    return ls;
}

}  // namespace

struct SyncService::Impl {
    fc::api::GmailClient* gmail = nullptr;
    QTimer* timer = nullptr;
    State state = State::Idle;
    QString lastError;
    bool busy = false;
    QString accountId;
};

SyncService::SyncService(fc::api::GmailClient* gmail, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->gmail = gmail;
    fc::cache::Database::initialize();
    // accountId starts empty; AccountContext / MainWindow drive it via
    // setAccountId() once the active account is known.
}

void SyncService::setAccountId(const QString& accountId) {
    qInfo("SyncService::setAccountId(this=%p): '%s' -> '%s'",
          static_cast<void*>(this),
          qUtf8Printable(d_->accountId), qUtf8Printable(accountId));
    d_->accountId = accountId;
}

QString SyncService::accountId() const { return d_->accountId; }

SyncService::State SyncService::state() const { return d_->state; }
QString SyncService::lastError() const { return d_->lastError; }

void SyncService::setState(State s) {
    if (d_->state == s) return;
    d_->state = s;
    emit stateChanged(s);
}

void SyncService::runOnce() {
    if (d_->busy) {
        qInfo("SyncService::runOnce(this=%p): busy, skip (accountId='%s')",
              static_cast<void*>(this),
              qUtf8Printable(d_->accountId));
        return;
    }
    if (d_->accountId.isEmpty()) {
        qInfo("SyncService::runOnce(this=%p): empty accountId, skip",
              static_cast<void*>(this));
        return;
    }
    d_->busy = true;

    const QString hid = fc::cache::MetaRepository::historyId(d_->accountId);
    qInfo("SyncService::runOnce: accountId='%s' hid='%s' → %s",
          qUtf8Printable(d_->accountId), qUtf8Printable(hid),
          hid.isEmpty() ? "doInitialSync" : "doIncrementalSync");
    if (hid.isEmpty()) {
        doInitialSync();
    } else {
        doIncrementalSync();
    }
}

void SyncService::startScheduler(int intervalMs) {
    if (!d_->timer) {
        d_->timer = new QTimer(this);
        connect(d_->timer, &QTimer::timeout, this, &SyncService::runOnce);
    }
    d_->timer->start(intervalMs);
}

void SyncService::stopScheduler() {
    if (d_->timer) d_->timer->stop();
}

void SyncService::doInitialSync() {
    setState(State::InitialSync);
    qInfo("SyncService::doInitialSync: starting getProfile (accountId='%s')",
          qUtf8Printable(d_->accountId));

    d_->gmail->getProfile(
        [this](fc::api::GmailClient::Profile p, fc::api::ApiError err) {
            if (err) {
                qWarning("SyncService::doInitialSync: getProfile failed: %s",
                         qUtf8Printable(err.message));
                d_->lastError = err.message;
                d_->busy = false;
                setState(State::Idle);
                emit failed(err.message);
                return;
            }
            qInfo("SyncService::doInitialSync: getProfile ok email='%s' "
                  "historyId='%s'",
                  qUtf8Printable(p.emailAddress),
                  qUtf8Printable(p.historyId));
            // Persist the canonical email under the active account's
            // account_meta sheet (account_meta replaced the global
            // meta.email row in v6).
            fc::cache::MetaRepository::set(d_->accountId,
                                           QStringLiteral("email"),
                                           p.emailAddress);
            emit profileFetched(p.emailAddress);
            // Don't record historyId yet — wait until the seed listing finishes
            // so a crash mid-init triggers a full retry rather than relying on
            // a delta we never fetched.

            d_->gmail->listLabels(
                [this, profileHid = p.historyId]
                (std::vector<fc::api::GmailClient::Label> labels, fc::api::ApiError lErr) {
                    if (lErr) {
                        d_->lastError = lErr.message;
                        d_->busy = false;
                        setState(State::Idle);
                        emit failed(lErr.message);
                        return;
                    }
                    for (const auto& l : labels) {
                        fc::cache::LabelRow row;
                        row.accountId = d_->accountId;
                        row.id        = l.id;
                        row.name      = l.name;
                        row.type      = l.type;
                        row.colorBg   = l.colorBg;
                        row.colorFg   = l.colorFg;
                        fc::cache::LabelRepository::upsert(d_->accountId, row);
                    }
                    emit labelsUpdated();

                    // List a chunk of message ids per seed label, dedupe, then
                    // serial-fetch full bodies (Gmail's HTTP/2 keeps this
                    // surprisingly cheap; Phase 4 swaps in real /batch).
                    auto pending = std::make_shared<QSet<QString>>();
                    auto remaining = std::make_shared<int>(seedLabels().size());

                    auto onPage = [this, pending, remaining, profileHid]
                        (fc::api::GmailClient::ListPage page, fc::api::ApiError pErr) {
                        if (!pErr) {
                            for (const auto& id : page.ids) pending->insert(id);
                        }
                        if (--(*remaining) > 0) return;

                        QStringList ids = pending->values();
                        if (ids.size() > kInitialPerLabelCap * 2)
                            ids = ids.mid(0, kInitialPerLabelCap * 2);
                        if (ids.isEmpty()) {
                            fc::cache::MetaRepository::setHistoryId(d_->accountId,
                                                                    profileHid);
                            d_->busy = false;
                            setState(State::Idle);
                            return;
                        }
                        // Commit profileHid only once the seed bodies
                        // are on disk. The previous "save now, fetch
                        // later" pattern left the user stranded after
                        // dropCache + premature exit: the next launch
                        // saw a non-empty historyId, did an incremental
                        // sync that found "no changes since profileHid",
                        // and silently left the inbox empty. Now an
                        // interrupted seed fetch means the next launch
                        // restarts from doInitialSync.
                        fetchAndStoreMessages(
                            ids, ids.size(), /*isInitial=*/true,
                            [accountId = d_->accountId, profileHid] {
                                fc::cache::MetaRepository::setHistoryId(
                                    accountId, profileHid);
                            });
                    };

                    for (const auto& label : seedLabels()) {
                        d_->gmail->listMessages(label, {}, {},
                                                kInitialPerLabelCap, onPage);
                    }
                });
        });
}

void SyncService::doIncrementalSync() {
    setState(State::IncrementalSync);

    const QString startId = fc::cache::MetaRepository::historyId(d_->accountId);
    d_->gmail->listHistory(startId, {},
        [this](fc::api::GmailClient::HistoryPage page, fc::api::ApiError err) {
            if (err) {
                d_->lastError = err.message;
                d_->busy = false;
                setState(State::Idle);
                emit failed(err.message);
                return;
            }
            if (page.historyTooOld) {
                // Smart resync: clear this account's historyId and let next
                // run do an initial sync.
                fc::cache::MetaRepository::setHistoryId(d_->accountId, {});
                d_->busy = false;
                setState(State::Idle);
                runOnce();
                return;
            }

            QSet<QString> added;
            QSet<QString> deleted;
            for (const auto& e : page.entries) {
                for (const auto& id : e.messagesAdded)   added.insert(id);
                for (const auto& id : e.messagesDeleted) deleted.insert(id);
            }
            // (We don't process labelAdded/labelRemoved deltas yet; the next
            // full message refetch picks up flag changes via upsert.)

            if (!page.historyId.isEmpty()) {
                fc::cache::MetaRepository::setHistoryId(d_->accountId,
                                                        page.historyId);
            }

            if (added.isEmpty() && deleted.isEmpty()) {
                d_->busy = false;
                setState(State::Idle);
                return;
            }

            // Note: deletions should be applied to the local cache too. We
            // keep the rows for now and only delete from listings via the
            // label edges. A Phase 4 evictor will clean up orphans.
            QStringList ids = added.values();
            fetchAndStoreMessages(ids, int(added.size()), /*isInitial=*/false);
        });
}

namespace {

// Per-account, per-label top-up state: where to resume the next
// listMessages page for that label. The key MUST include accountId
// so two signed-in accounts walking the same label don't share /
// corrupt each other's cursor — without this, account A's saved
// SENT pageToken (pointing at, say, 2004 messages) would get
// reused when account B clicks SENT, returning ancient ids that
// land in B's cache and produce a 22-year gap in the display
// between B's recent SENT messages and the wrong ones from A's
// history. Stored in account_meta (cascades on dropCache) instead
// of the global meta sheet so a per-account cache wipe also
// resets the cursor.
QString topUpTokenKey(const QString& labelId) {
    return QStringLiteral("labelTopUp/%1/pageToken").arg(labelId);
}

// Marks the label as fully walked once the server returns no more
// pages, so the next click resets to the start instead of yielding
// nothing. 500 is the Gmail API's per-page maximum — we use the
// largest single page to minimise the number of round-trips a user
// has to scroll-trigger before a deep label finishes catching up.
constexpr int kTopUpPageSize = 500;

}  // namespace

void SyncService::topUpLabel(const QString& labelId) {
    if (labelId.isEmpty()) {
        qInfo("topUpLabel(account=%s): empty labelId, skip",
              qUtf8Printable(d_->accountId));
        return;
    }
    if (d_->busy) {
        qInfo("topUpLabel(account=%s, label=%s): busy, skip",
              qUtf8Printable(d_->accountId), qUtf8Printable(labelId));
        return;
    }
    qInfo("topUpLabel(account=%s, label=%s): starting",
          qUtf8Printable(d_->accountId), qUtf8Printable(labelId));
    emit topUpStarted(labelId);
    topUpLabelStep(labelId, /*pagesWalked=*/0, /*totalStoredSoFar=*/0);
}

void SyncService::topUpLabelStep(const QString& labelId,
                                  int pagesWalked,
                                  int totalStoredSoFar) {
    // Bound the auto-walk so a fully-cached label can't loop the
    // sync indefinitely. Once we've walked the cap, surface
    // topUpFinished and let the next user scroll resume from where
    // the saved pageToken landed.
    constexpr int kMaxPagesPerTopUp = 8;
    if (d_->busy) {
        // Between our previous step clearing busy and this scheduled
        // step running, something else (incremental sync, another
        // top-up) grabbed the slot. Close the topUp bracket so the
        // UI doesn't get stuck in "Loading more…" — the user can
        // scroll again to retry once the current sync settles.
        emit topUpFinished(labelId, totalStoredSoFar,
                            /*serverExhausted=*/false);
        return;
    }
    d_->busy = true;
    setState(State::IncrementalSync);

    const QString tokenKey = topUpTokenKey(labelId);
    // Per-account scoping: store/load the cursor in account_meta
    // (cascades on dropCache + isolates accounts). The previous
    // implementation used global meta which let two accounts walking
    // the same label clobber each other's cursor.
    const QString pageToken = fc::cache::MetaRepository::get(d_->accountId,
                                                              tokenKey);

    // The synthetic "__all_mail" view passes through here too. Gmail's
    // messages.list with no labelIds filter (the empty-string case)
    // returns the full mailbox excluding SPAM/TRASH by default —
    // exactly the All Mail semantics.
    const QString gmailLabelArg = (labelId == QStringLiteral("__all_mail"))
        ? QString() : labelId;

    QPointer<SyncService> self(this);
    d_->gmail->listMessages(gmailLabelArg, /*q=*/QString(), pageToken,
        kTopUpPageSize,
        [self, labelId, tokenKey, pagesWalked, totalStoredSoFar]
        (fc::api::GmailClient::ListPage page, fc::api::ApiError err) {
            if (!self) return;   // we were destroyed in flight
            if (err) {
                // Top-up is best-effort. The previous code refused to
                // advance the cursor on failure ("retry the same
                // window") — but if the saved token is itself the
                // problem (stale from another account, opaque-token
                // refresh, etc.), the next attempt would fail the
                // same way and we'd be stuck. Clear the cursor so
                // the next top-up restarts at the most-recent page.
                qWarning("topUpLabel: listMessages failed for label='%s' "
                         "(http=%d) — clearing cursor",
                         qUtf8Printable(labelId), err.httpStatus);
                fc::cache::MetaRepository::set(self->d_->accountId,
                                                tokenKey, QString());
                self->d_->busy = false;
                self->setState(State::Idle);
                emit self->topUpFinished(labelId, totalStoredSoFar,
                                          /*serverExhausted=*/false);
                return;
            }
            const bool serverExhausted = page.nextPageToken.isEmpty();

            QStringList missing;
            missing.reserve(page.ids.size());
            for (const auto& id : page.ids) {
                if (!fc::cache::MessageRepository::exists(
                        self->d_->accountId, id)) {
                    missing << id;
                }
            }

            if (missing.isEmpty()) {
                // Whole page already cached. Advance the cursor and
                // either keep walking (so the user actually sees
                // progress on labels like "All Mail" where the most
                // recent thousand ids have all been seeded by other
                // labels' syncs already) or settle if we've hit the
                // safety cap / server end.
                fc::cache::MetaRepository::set(self->d_->accountId,
                                                tokenKey, page.nextPageToken);
                self->d_->busy = false;
                self->setState(State::Idle);
                emit self->messagesUpdated();
                const bool capReached = pagesWalked + 1 >= kMaxPagesPerTopUp;
                if (serverExhausted || capReached) {
                    emit self->topUpFinished(labelId, totalStoredSoFar,
                                              serverExhausted);
                    return;
                }
                // Re-enter on the event loop so we don't blow the
                // stack and so other queued events (UI repaint,
                // scroll handling) still get a chance to run between
                // pages.
                QPointer<SyncService> selfCopy = self;
                const QString lbl = labelId;
                const int pw = pagesWalked + 1;
                const int ts = totalStoredSoFar;
                QTimer::singleShot(0, self.data(),
                    [selfCopy, lbl, pw, ts] {
                        if (!selfCopy) return;
                        selfCopy->topUpLabelStep(lbl, pw, ts);
                    });
                return;
            }
            // Page has uncached ids — fetch them, then settle.
            // (We don't continue auto-walking after a successful
            // fetch; one server round-trip's worth of new rows is
            // plenty for a single top-up. The user can scroll past
            // the new rows to trigger the next walk.)
            const int storeCount = static_cast<int>(missing.size());
            const QString nextToken = page.nextPageToken;
            const int storedAfter = totalStoredSoFar + storeCount;
            self->fetchAndStoreMessages(missing, /*newCount=*/0,
                /*isInitial=*/false,
                [self, labelId, tokenKey, nextToken, storedAfter,
                 serverExhausted] {
                    if (!self) return;
                    fc::cache::MetaRepository::set(self->d_->accountId,
                                                    tokenKey, nextToken);
                    emit self->topUpFinished(labelId, storedAfter,
                                              serverExhausted);
                });
        });
}

void SyncService::fetchAndStoreMessages(const QStringList& ids,
                                        int newCount,
                                        bool isInitial,
                                        std::function<void()> done) {
    if (ids.isEmpty()) {
        d_->busy = false;
        setState(State::Idle);
        if (newCount > 0 && !isInitial) emit newMessages(newCount);
        if (done) done();
        return;
    }

    // QPointer guard threaded through every async callback so a
    // SyncService destroyed mid-flight (shutdown, sign-out) doesn't
    // dereference a dead `d_->gmail` / `d_->busy` or emit signals
    // on a freed object. The earlier topUpLabel guard was useless if
    // the inner onOne / next captured `this` raw — fixed here.
    QPointer<SyncService> self(this);

    auto remaining = std::make_shared<int>(ids.size());
    auto stored    = std::make_shared<int>(0);
    // Accumulate fetched messages and commit them in ONE SQLite
    // transaction at the end. The previous code did one
    // db.transaction()/commit() PER message, which fsyncs the WAL
    // per call — for a 400-message initial seed listing that's
    // hundreds of milliseconds of UI-thread blocking on disk I/O.
    // Batching one transaction across the batch trades that cost for
    // a single fsync and lets the event loop run between network
    // callbacks (where setHtml / status-bar / placeholder repaints
    // can actually land).
    auto pending = std::make_shared<std::vector<fc::Message>>();
    pending->reserve(ids.size());

    auto onOne = [self, remaining, stored, pending,
                   newCount, isInitial,
                   done = std::move(done)]
        (fc::Message m, fc::api::ApiError err) {
        // We may have been destroyed mid-flight. If self is gone we
        // skip the per-message work; the completion callback below
        // still runs once all remaining counters hit 0 so the caller
        // can settle its own state (topUpFinished, etc.).
        if (!err && !m.id.isEmpty() && self) {
            m.accountId = self->d_->accountId;
            pending->push_back(std::move(m));
        }
        if (--(*remaining) > 0) return;
        if (!self) {
            if (done) done();
            return;
        }
        // All getMessage callbacks have returned. Commit the batch
        // in a single transaction. upsertMany returns the number of
        // rows successfully stored.
        *stored = fc::cache::MessageRepository::upsertMany(
            self->d_->accountId, *pending);
        fc::cache::LabelRepository::recomputeCounts(self->d_->accountId);
        emit self->labelsUpdated();
        emit self->messagesUpdated();
        if (!isInitial && newCount > 0) emit self->newMessages(newCount);
        self->d_->busy = false;
        self->setState(State::Idle);
        if (done) done();
    };

    // Concurrency limiter: kick off kSerialGetBatch and queue the rest.
    auto queue = std::make_shared<QStringList>(ids);
    auto inflight = std::make_shared<int>(0);

    // Function that pulls the next id and dispatches itself again
    // when each callback fires. The original code captured `next`
    // (the shared_ptr to the std::function) BY VALUE inside the
    // outer lambda body itself, producing a reference cycle:
    //   shared_ptr → std::function (lambda) → captures shared_ptr
    // The outer lambda kept itself alive forever, leaking the chain
    // even after all inflight callbacks completed. Fix:
    //   - The outer lambda captures only a weak_ptr (no own ref).
    //   - Each inner getMessage callback captures a strong shared
    //     (via lock), anchoring the chain to the in-flight requests.
    // Once every inner callback resolves and drops its strong copy,
    // the std::function reaches refcount zero and is freed cleanly.
    auto next = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> nextWeak = next;
    *next = [self, queue, inflight, onOne, nextWeak]() {
        if (!self) return;   // service died; abandon the queue
        while (*inflight < kSerialGetBatch && !queue->isEmpty()) {
            const QString id = queue->takeFirst();
            ++(*inflight);
            auto strong = nextWeak.lock();
            self->d_->gmail->getMessage(id,
                [inflight, strong, onOne]
                (fc::Message m, fc::api::ApiError err) {
                    --(*inflight);
                    onOne(m, err);
                    if (strong) (*strong)();
                });
        }
    };
    (*next)();
}

}  // namespace fc::sync
