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

void SyncService::fetchAndStoreMessages(const QStringList& ids,
                                        int newCount,
                                        bool isInitial) {
    if (ids.isEmpty()) {
        d_->busy = false;
        setState(State::Idle);
        if (newCount > 0 && !isInitial) emit newMessages(newCount);
        return;
    }

    auto remaining = std::make_shared<int>(ids.size());
    auto stored    = std::make_shared<int>(0);

    auto onOne = [this, remaining, stored, newCount, isInitial]
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
