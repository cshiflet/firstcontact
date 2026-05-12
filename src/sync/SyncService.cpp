#include "SyncService.h"

#include "api/GmailClient.h"
#include "cache/Database.h"
#include "cache/LabelRepository.h"
#include "cache/MessageRepository.h"
#include "cache/MetaRepository.h"
#include "cache/Migrations.h"   // for fc::cache::databaseHandle()
#include "util/PageSizePref.h"

#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace fc::sync {

namespace {

constexpr int kInitialPerLabelCap = 200;       // first-run pages cap
constexpr int kSerialGetBatch     = 25;        // concurrent in-flight gets
constexpr int kBatchChunkSize     = 50;        // Gmail /batch sub-requests per chunk

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

// QSettings-backed read of the low-bandwidth-mode preference. The pref
// is defined in fc::ui::Preferences, but fc_sync sits below fc_ui in
// the link graph, so we read the underlying QSettings key directly.
// Key must stay in sync with Preferences::kLowBandwidthKey.
bool lowBandwidthModeEnabled() {
    QSettings s;
    return s.value(QStringLiteral("sync/lowBandwidthMode"), false).toBool();
}

// Per-account, per-label "we've walked this label end-to-end at least
// once" flag. Lives in account_meta so dropCache resets it. Key shape
// matches what topUpTokenKey uses; the crawler skips labels whose
// flag is "1".
QString crawlExhaustedKey(const QString& labelId) {
    return QStringLiteral("labelCrawl/%1/exhausted").arg(labelId);
}

}  // namespace

struct SyncService::Impl {
    fc::api::GmailClient* gmail = nullptr;
    QTimer* timer = nullptr;
    QTimer* crawlTimer = nullptr;
    State state = State::Idle;
    QString lastError;
    bool busy = false;
    QString accountId;
    // Latches compressionPromptDue to once per SyncService instance.
    // MainWindow does the further gating (Preferences flags, dict
    // presence) before actually showing the dialog.
    bool compressionPromptEmitted = false;

    // cacheLabelComplete walk state. cacheLabelTarget holds the
    // label being walked (empty = no walk in flight). cacheLabelCancel
    // is a request flag the user can set via cancelCacheLabel; the
    // chain checks it at each topUpFinished to decide whether to
    // continue or finalize. cacheLabelTotalStored sums stored rows
    // across pages so the final cacheLabelFinished payload has the
    // right number.
    QString cacheLabelTarget;
    bool    cacheLabelCancel = false;
    int     cacheLabelTotalStored = 0;
};

SyncService::SyncService(fc::api::GmailClient* gmail, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->gmail = gmail;
    fc::cache::Database::initialize();
    // accountId starts empty; AccountContext / MainWindow drive it via
    // setAccountId() once the active account is known.

    // cacheLabelComplete chain: when a top-up wraps up for the label
    // we're walking, either fire the next page (server has more) or
    // settle (server exhausted / user cancelled). Self-connect runs
    // directly (same object, same thread).
    connect(this, &SyncService::topUpFinished, this,
        [this](const QString& labelId, int newRowsStored,
                bool serverExhausted) {
            if (d_->cacheLabelTarget.isEmpty()) return;
            if (labelId != d_->cacheLabelTarget) return;
            d_->cacheLabelTotalStored += newRowsStored;
            emit cacheLabelProgress(labelId, d_->cacheLabelTotalStored);
            const bool cancelled = d_->cacheLabelCancel;
            if (cancelled || serverExhausted) {
                const int total = d_->cacheLabelTotalStored;
                const QString lbl = d_->cacheLabelTarget;
                d_->cacheLabelTotalStored = 0;
                d_->cacheLabelTarget.clear();
                d_->cacheLabelCancel = false;
                emit cacheLabelFinished(lbl, total, cancelled);
                return;
            }
            // Queued post so we don't re-enter topUpLabel from
            // inside its own emit chain.
            QPointer<SyncService> self(this);
            const QString lbl = labelId;
            QMetaObject::invokeMethod(this, [self, lbl] {
                if (!self) return;
                if (self->d_->cacheLabelTarget != lbl) return;
                if (self->d_->cacheLabelCancel) return;
                self->topUpLabel(lbl);
            }, Qt::QueuedConnection);
        });
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

// Per-page batch size for Gmail's messages.list during scroll-driven
// top-up. Read from the same QSettings key the message list pageSize()
// uses (Preferences::messagePageSize) so the user has one "messages
// per batch" knob that governs both the local cache page reads and
// the server-side fetch. 500 is the Gmail API's per-page maximum.
int topUpPageSize() { return fc::util::messagePageSize(); }

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
    qInfo("topUpLabel(account=%s, label=%s): starting (pageSize=%d)",
          qUtf8Printable(d_->accountId), qUtf8Printable(labelId),
          topUpPageSize());
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
        topUpPageSize(),
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
                                        std::function<void()> done,
                                        const QString& bodyFormat) {
    if (ids.isEmpty()) {
        d_->busy = false;
        setState(State::Idle);
        if (newCount > 0 && !isInitial) emit newMessages(newCount);
        if (done) done();
        return;
    }

    // Resolve the effective body format. An explicit caller-supplied
    // value wins; otherwise we honour the low-bandwidth pref. The
    // metadata path uses /batch (one HTTP round-trip per chunk) and
    // produces rows with empty body_text / body_html — bodies arrive
    // later via fetchBodyOnDemand when the user opens a message.
    const QString fmt = bodyFormat.isEmpty()
        ? (lowBandwidthModeEnabled() ? QStringLiteral("metadata")
                                      : QStringLiteral("full"))
        : bodyFormat;

    // QPointer guard threaded through every async callback so a
    // SyncService destroyed mid-flight (shutdown, sign-out) doesn't
    // dereference a dead `d_->gmail` / `d_->busy` or emit signals
    // on a freed object. The earlier topUpLabel guard was useless if
    // the inner onOne / next captured `this` raw — fixed here.
    QPointer<SyncService> self(this);

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

    auto doneSp = std::make_shared<std::function<void()>>(std::move(done));
    auto onDoneAll = [self, pending, newCount, isInitial, doneSp]() {
        if (!self) {
            if (*doneSp) (*doneSp)();
            return;
        }
        const int storedCount = fc::cache::MessageRepository::upsertMany(
            self->d_->accountId, *pending);
        Q_UNUSED(storedCount);
        fc::cache::LabelRepository::recomputeCounts(self->d_->accountId);
        emit self->labelsUpdated();
        emit self->messagesUpdated();
        if (!isInitial && newCount > 0) emit self->newMessages(newCount);

        // Compression-prompt threshold: once an account accumulates
        // enough bodies for a meaningful dictionary AND no dict has
        // been trained yet, nudge MainWindow to ask the user
        // whether to compress. Latched per-process so we don't spam
        // the signal every batch.
        if (!self->d_->compressionPromptEmitted) {
            const QString aid = self->d_->accountId;
            const QByteArray dict =
                fc::cache::MessageRepository::dictionaryFor(aid);
            if (dict.isEmpty()) {
                constexpr int kPromptThreshold = 200;
                const int bodies =
                    fc::cache::MessageRepository::bodyCountFor(aid);
                if (bodies >= kPromptThreshold) {
                    self->d_->compressionPromptEmitted = true;
                    emit self->compressionPromptDue(bodies);
                }
            }
        }

        self->d_->busy = false;
        self->setState(State::Idle);
        if (*doneSp) (*doneSp)();
    };

    // Serial-getMessage dispatch helper. Used by:
    //   - The legacy non-metadata path (fmt == "full").
    //   - The /batch error fallback in the metadata path below.
    // Pulls each id with its own getMessage(id, fmt, cb) round-trip
    // up to kSerialGetBatch in flight at once. Wrapped in a
    // shared_ptr<std::function> so the metadata-path closure can
    // capture and invoke it.
    auto dispatchSerial = std::make_shared<
        std::function<void(QStringList, QString)>>();
    *dispatchSerial = [self, pending, onDoneAll]
        (QStringList serialIds, QString fmtArg) {
        if (serialIds.isEmpty()) { onDoneAll(); return; }
        auto remaining = std::make_shared<int>(serialIds.size());
        auto onOne = [self, remaining, pending, onDoneAll]
            (fc::Message m, fc::api::ApiError err) {
            if (!err && !m.id.isEmpty() && self) {
                m.accountId = self->d_->accountId;
                pending->push_back(std::move(m));
            }
            if (--(*remaining) > 0) return;
            onDoneAll();
        };
        auto queue = std::make_shared<QStringList>(std::move(serialIds));
        auto inflight = std::make_shared<int>(0);
        // weak_ptr → std::function cycle break. See the older
        // legacy-path comment (pre-refactor) for the full rationale.
        auto next = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> nextWeak = next;
        *next = [self, queue, inflight, onOne, nextWeak, fmtArg]() {
            if (!self) return;
            while (*inflight < kSerialGetBatch && !queue->isEmpty()) {
                const QString id = queue->takeFirst();
                ++(*inflight);
                auto strong = nextWeak.lock();
                self->d_->gmail->getMessage(id, fmtArg,
                    [inflight, strong, onOne]
                    (fc::Message m, fc::api::ApiError err) {
                        --(*inflight);
                        onOne(m, err);
                        if (strong) (*strong)();
                    });
            }
        };
        (*next)();
    };

    if (fmt == QStringLiteral("metadata")) {
        // Batch path. Chunk ids into ≤kBatchChunkSize sub-requests per
        // /batch round-trip; chain chunks serially via a tail-call-
        // style recursion to keep the cumulative pending vector
        // intact across chunks. One upsertMany commit at the end.
        auto queue = std::make_shared<QStringList>(ids);

        auto fireChunk = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> chunkWeak = fireChunk;
        *fireChunk = [self, queue, pending, onDoneAll, chunkWeak,
                      dispatchSerial]() {
            if (!self) { onDoneAll(); return; }
            if (queue->isEmpty()) { onDoneAll(); return; }
            QStringList chunk;
            const int take = qMin(kBatchChunkSize, queue->size());
            for (int i = 0; i < take; ++i) chunk << queue->takeFirst();

            auto strong = chunkWeak.lock();
            self->d_->gmail->batchGetMessages(chunk,
                QStringLiteral("metadata"),
                [self, pending, onDoneAll, strong, queue, chunk,
                 dispatchSerial]
                (std::vector<fc::api::GmailClient::BatchMessageResult> results,
                 fc::api::ApiError err) {
                    if (!self) { onDoneAll(); return; }
                    if (err) {
                        // Whole-batch failure (network / boundary
                        // parse / Gmail returned a JSON error envelope
                        // instead of multipart). Fall back to the
                        // serial getMessage path for THIS chunk plus
                        // every still-queued id. Slower (one HTTP
                        // request per id instead of one per ~50) but
                        // the user sees their messages instead of an
                        // empty list while we figure out why /batch
                        // didn't like our request.
                        QStringList unfetched = chunk;
                        while (!queue->isEmpty()) {
                            unfetched << queue->takeFirst();
                        }
                        qWarning("fetchAndStoreMessages: batch failed "
                                 "(%s) — falling back to serial "
                                 "getMessage for %d id(s)",
                                 qUtf8Printable(err.message),
                                 int(unfetched.size()));
                        (*dispatchSerial)(std::move(unfetched),
                                          QStringLiteral("metadata"));
                        return;
                    }
                    for (auto& r : results) {
                        if (!r.err && !r.msg.id.isEmpty()) {
                            r.msg.accountId = self->d_->accountId;
                            pending->push_back(std::move(r.msg));
                        }
                    }
                    if (strong) (*strong)();
                    else onDoneAll();
                });
        };
        (*fireChunk)();
        return;
    }

    // Legacy serial-get path (fmt == "full" or any explicit
    // bodyFormat). Same dispatcher; just enters here directly.
    (*dispatchSerial)(ids, fmt);
}

void SyncService::fetchBodyOnDemand(const QString& messageId,
                                     std::function<void(fc::api::ApiError)> cb) {
    if (messageId.isEmpty() || d_->accountId.isEmpty()) {
        if (cb) cb(fc::api::ApiError{
            fc::api::ApiErrorKind::BadRequest, 0,
            QStringLiteral("fetchBodyOnDemand: empty id or account"), {}});
        return;
    }
    QPointer<SyncService> self(this);
    const QString accountId = d_->accountId;
    d_->gmail->getMessage(messageId, QStringLiteral("full"),
        [self, accountId, cb = std::move(cb)]
        (fc::Message m, fc::api::ApiError err) {
            if (err) { if (cb) cb(err); return; }
            if (!self) return;
            m.accountId = accountId;
            fc::cache::MessageRepository::upsert(accountId, m);
            emit self->messagesUpdated();
            if (cb) cb({});
        });
}

void SyncService::cacheLabelComplete(const QString& labelId) {
    if (labelId.isEmpty()) return;
    if (!d_->cacheLabelTarget.isEmpty()) {
        qInfo("SyncService::cacheLabelComplete: walk already in flight "
              "for label '%s'; ignoring '%s'",
              qUtf8Printable(d_->cacheLabelTarget),
              qUtf8Printable(labelId));
        return;
    }
    qInfo("SyncService::cacheLabelComplete: starting walk of label '%s'",
          qUtf8Printable(labelId));
    d_->cacheLabelTarget = labelId;
    d_->cacheLabelCancel = false;
    d_->cacheLabelTotalStored = 0;
    topUpLabel(labelId);
}

void SyncService::cancelCacheLabel() {
    if (d_->cacheLabelTarget.isEmpty()) return;
    qInfo("SyncService::cancelCacheLabel: requesting cancel of walk of "
          "'%s' at %d row(s) stored",
          qUtf8Printable(d_->cacheLabelTarget),
          d_->cacheLabelTotalStored);
    // Flag the cancel; the in-flight top-up runs its current page
    // to completion. When it fires topUpFinished, the chain handler
    // sees the flag and finalizes with cancelled=true.
    d_->cacheLabelCancel = true;
}

void SyncService::tickBackgroundCrawl() {
    if (d_->accountId.isEmpty()) return;
    // Never race with foreground work. busy covers active sync /
    // top-up; cacheLabelTarget covers an in-flight user-requested
    // walk. Either way, defer until the next tick.
    if (d_->busy) return;
    if (!d_->cacheLabelTarget.isEmpty()) return;

    // Pick the next label to advance. Strategy: walk every label in
    // alphabetical id order (deterministic, no QHash randomness),
    // skip those already exhausted, pick the first hit. When every
    // label is exhausted we just no-op — the user has to hit "Reset
    // crawl progress" to revisit.
    QStringList labelIds;
    {
        auto db = fc::cache::databaseHandle();
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT id FROM labels WHERE account_id = :a ORDER BY id"));
        q.bindValue(QStringLiteral(":a"), d_->accountId);
        if (q.exec()) {
            while (q.next()) labelIds << q.value(0).toString();
        }
    }
    if (labelIds.isEmpty()) return;

    QString pick;
    for (const QString& id : labelIds) {
        const QString flag = fc::cache::MetaRepository::get(
            d_->accountId, crawlExhaustedKey(id));
        if (flag == QStringLiteral("1")) continue;
        pick = id;
        break;
    }
    if (pick.isEmpty()) {
        // Every label is marked exhausted. Don't auto-reset — the user
        // either explicitly wants the cache to plateau (default) or
        // pushes the reset button when they want another pass.
        return;
    }

    // Self-mark "exhausted" by listening once for the next topUpFinished
    // on this label. The handler also fires for foreground top-ups, but
    // we only set the flag when the server actually returned no more
    // pages (serverExhausted==true). A QPointer guards a destroyed
    // service mid-flight.
    QPointer<SyncService> self(this);
    const QString lbl = pick;
    auto* conn = new QMetaObject::Connection;
    *conn = connect(this, &SyncService::topUpFinished, this,
        [self, lbl, conn](const QString& labelId, int /*stored*/,
                            bool serverExhausted) {
            if (!self) { delete conn; return; }
            if (labelId != lbl) return;
            if (serverExhausted) {
                fc::cache::MetaRepository::set(self->d_->accountId,
                                                crawlExhaustedKey(lbl),
                                                QStringLiteral("1"));
            }
            QObject::disconnect(*conn);
            delete conn;
        });

    qInfo("SyncService::tickBackgroundCrawl: advancing label '%s' "
          "(account='%s')",
          qUtf8Printable(pick), qUtf8Printable(d_->accountId));
    topUpLabel(pick);
}

void SyncService::configureBackgroundCrawl(bool enabled, int intervalSec) {
    const int seconds = qMax(1, intervalSec);
    if (!enabled) {
        if (d_->crawlTimer) d_->crawlTimer->stop();
        return;
    }
    if (!d_->crawlTimer) {
        d_->crawlTimer = new QTimer(this);
        connect(d_->crawlTimer, &QTimer::timeout, this,
                &SyncService::tickBackgroundCrawl);
    }
    d_->crawlTimer->start(seconds * 1000);
}

void SyncService::resetBackgroundCrawlProgress() {
    if (d_->accountId.isEmpty()) return;
    auto db = fc::cache::databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "DELETE FROM account_meta "
        "WHERE account_id = :a AND key LIKE 'labelCrawl/%/exhausted'"));
    q.bindValue(QStringLiteral(":a"), d_->accountId);
    if (!q.exec()) {
        qWarning("resetBackgroundCrawlProgress: DELETE failed: %s",
                 qUtf8Printable(q.lastError().text()));
    } else {
        qInfo("SyncService::resetBackgroundCrawlProgress: cleared crawl "
              "flags for account '%s'",
              qUtf8Printable(d_->accountId));
    }
}

}  // namespace fc::sync
