#include "SyncService.h"

#include "api/GmailClient.h"
#include "cache/Database.h"
#include "cache/LabelRepository.h"
#include "cache/MessageRepository.h"
#include "cache/MetaRepository.h"

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
};

SyncService::SyncService(fc::api::GmailClient* gmail, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->gmail = gmail;
    fc::cache::Database::initialize();
}

SyncService::State SyncService::state() const { return d_->state; }
QString SyncService::lastError() const { return d_->lastError; }

void SyncService::setState(State s) {
    if (d_->state == s) return;
    d_->state = s;
    emit stateChanged(s);
}

void SyncService::runOnce() {
    if (d_->busy) return;
    d_->busy = true;

    const QString hid = fc::cache::MetaRepository::historyId();
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

    d_->gmail->getProfile(
        [this](fc::api::GmailClient::Profile p, fc::api::ApiError err) {
            if (err) {
                d_->lastError = err.message;
                d_->busy = false;
                setState(State::Idle);
                emit failed(err.message);
                return;
            }
            fc::cache::MetaRepository::set(QStringLiteral("email"), p.emailAddress);
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
                        row.id      = l.id;
                        row.name    = l.name;
                        row.type    = l.type;
                        row.colorBg = l.colorBg;
                        row.colorFg = l.colorFg;
                        fc::cache::LabelRepository::upsert(row);
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
                            fc::cache::MetaRepository::setHistoryId(profileHid);
                            d_->busy = false;
                            setState(State::Idle);
                            return;
                        }
                        // After fetches finish we'll commit the historyId.
                        fetchAndStoreMessages(ids, ids.size(), /*isInitial=*/true);
                        // Save profileHid via meta now; if we crash mid-fetch
                        // the next run does an incremental sync from this id
                        // (Gmail's history is stable).
                        fc::cache::MetaRepository::setHistoryId(profileHid);
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

    const QString startId = fc::cache::MetaRepository::historyId();
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
                // Smart resync: clear the historyId and let next run do an
                // initial sync. Phase 2 keeps it simple; Phase 4 diffs IDs
                // per label rather than refetching everything.
                fc::cache::MetaRepository::setHistoryId({});
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
                fc::cache::MetaRepository::setHistoryId(page.historyId);
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

// Per-label top-up state: where to resume the next listMessages page
// for that label. We key the meta column with a label-scoped prefix so
// every label gets its own page-walk independent of the others.
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
    if (labelId.isEmpty()) return;
    // Skip if a sync is already in progress — top-up shares the same
    // busy / state machinery as incremental sync, so two paths racing
    // each other to set State and emit messagesUpdated would scramble
    // the FSM. The user can click the label again once the in-flight
    // sync settles.
    if (d_->busy) return;
    // Skip the round-trip for labels that incremental sync already
    // keeps up to date; they're guaranteed complete from the initial
    // seed onward.
    for (const auto& seed : seedLabels()) {
        if (seed == labelId) return;
    }

    emit topUpStarted(labelId);

    const QString tokenKey = topUpTokenKey(labelId);
    const QString pageToken = fc::cache::MetaRepository::get(tokenKey);

    d_->gmail->listMessages(labelId, /*q=*/QString(), pageToken,
        kTopUpPageSize,
        [this, labelId, tokenKey]
        (fc::api::GmailClient::ListPage page, fc::api::ApiError err) {
            if (err) {
                // Don't escalate — top-up is best-effort. The user
                // still sees whatever's in the cache.
                emit topUpFinished(labelId, 0, /*serverExhausted=*/false);
                return;
            }
            // Save the next page token (or clear it when we ran off
            // the end, so the next click starts fresh from page 1).
            fc::cache::MetaRepository::set(tokenKey, page.nextPageToken);
            const bool serverExhausted = page.nextPageToken.isEmpty();

            // Skip ids we already have in the cache to avoid a wasted
            // getMessage round-trip per known message.
            QStringList missing;
            missing.reserve(page.ids.size());
            for (const auto& id : page.ids) {
                if (!fc::cache::MessageRepository::exists(id)) {
                    missing << id;
                }
            }
            if (missing.isEmpty()) {
                // Nothing new from the server, but still emit so the
                // UI can refresh in case prior async writes have
                // landed since the last reload.
                emit messagesUpdated();
                emit topUpFinished(labelId, 0, serverExhausted);
                return;
            }
            // fetchAndStoreMessages flips d_->busy and the FSM state
            // — fine: top-up doubles as a small sync pass and
            // benefits from the same guard against overlapping work.
            // The completion callback fires AFTER all per-message
            // getMessage round-trips have settled, so the
            // topUpFinished signal lines up with the moment the user
            // actually has the new rows.
            d_->busy = true;
            setState(State::IncrementalSync);
            const int storeCount = static_cast<int>(missing.size());
            fetchAndStoreMessages(missing, /*newCount=*/0, /*isInitial=*/false,
                [this, labelId, storeCount, serverExhausted] {
                    emit topUpFinished(labelId, storeCount, serverExhausted);
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

    auto remaining = std::make_shared<int>(ids.size());
    auto stored    = std::make_shared<int>(0);

    auto onOne = [this, remaining, stored, newCount, isInitial,
                   done = std::move(done)]
        (fc::Message m, fc::api::ApiError err) {
        if (!err && !m.id.isEmpty()) {
            fc::cache::MessageRepository::upsert(m);
            ++(*stored);
        }
        if (--(*remaining) > 0) return;
        fc::cache::LabelRepository::recomputeCounts();
        emit labelsUpdated();
        emit messagesUpdated();
        if (!isInitial && newCount > 0) emit newMessages(newCount);
        d_->busy = false;
        setState(State::Idle);
        if (done) done();
    };

    // Concurrency limiter: kick off kSerialGetBatch and queue the rest.
    auto queue = std::make_shared<QStringList>(ids);
    auto inflight = std::make_shared<int>(0);

    // Function that pulls the next id and dispatches.
    auto next = std::make_shared<std::function<void()>>();
    *next = [this, queue, inflight, onOne, next]() mutable {
        while (*inflight < kSerialGetBatch && !queue->isEmpty()) {
            const QString id = queue->takeFirst();
            ++(*inflight);
            d_->gmail->getMessage(id,
                [inflight, next, onOne]
                (fc::Message m, fc::api::ApiError err) {
                    --(*inflight);
                    onOne(m, err);
                    (*next)();
                });
        }
    };
    (*next)();
}

}  // namespace fc::sync
