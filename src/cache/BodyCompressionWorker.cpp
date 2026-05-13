#include "BodyCompressionWorker.h"

#include "Database.h"
#include "MessageRepository.h"
#include "Migrations.h"   // databaseHandle()
#include "util/BodyCodec.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <vector>

namespace fc::cache {

namespace {

// Number of rows sampled to train the dictionary. zstd's dict
// algorithm prefers many small samples to a few large ones. Stratified
// random selection ensures the sample spans both old (frequently
// system-mail) and recent (newsletters / personal) bodies.
constexpr int kTrainingSampleCount = 1000;

// Per-sample size cap to bound RAM during training. A 32 KiB cap is
// generous — the upper percentile of email bodies sits well below
// that — and prevents one giant marketing PDF-converted-to-HTML
// from blowing the training memory budget.
constexpr int kTrainingPerSampleCap = 32 * 1024;

// Backfill chunk size. 100 rows per UPDATE transaction keeps WAL
// growth bounded (~5-15 MB) and gives the SQLite checkpoint thread
// natural breakpoints between commits. Smaller chunks mean more
// fsyncs; larger chunks mean bigger transient WAL. 100 is a healthy
// middle.
constexpr int kBackfillChunkSize = 100;

// Target output dict size. Bigger dicts capture more patterns but
// cost more decompression init. 32 KiB is a sweet spot for mixed
// email content (ratio gains plateau past ~64 KiB on test corpora).
constexpr int kDictSizeBytes = 32 * 1024;

QByteArray utf8OrBytes(const QVariant& v) {
    // body_text / body_html storage is variable: plaintext rows come
    // back as String, compressed rows as ByteArray. Always coerce to
    // bytes so we can feed BodyCodec consistently.
    if (v.userType() == QMetaType::QByteArray) return v.toByteArray();
    return v.toString().toUtf8();
}

}  // namespace

BodyCompressionWorker::BodyCompressionWorker(const QString& accountId,
                                              Mode mode,
                                              QObject* parent)
    : QObject(parent), accountId_(accountId), mode_(mode),
      thread_(new QThread()) {
    thread_->setObjectName(QStringLiteral("fc-compress-") + accountId);
    moveToThread(thread_);
    // Ensure deletion happens on thread teardown.
    connect(thread_, &QThread::finished, this, &QObject::deleteLater);
    connect(this, &QObject::destroyed, thread_, &QObject::deleteLater);
}

BodyCompressionWorker::~BodyCompressionWorker() = default;

void BodyCompressionWorker::start() {
    thread_->start();
    QMetaObject::invokeMethod(this, &BodyCompressionWorker::doWork,
                               Qt::QueuedConnection);
}

void BodyCompressionWorker::doWork() {
    if (accountId_.isEmpty()) {
        emit failed(accountId_,
                     QStringLiteral("compression: empty accountId"));
        thread_->quit();
        return;
    }

    // SQLite connections in Qt are per-thread; the worker's QThread
    // has no connection until we register one. Database::initialize
    // is idempotent — if this thread already has the connection
    // (re-run after a thread reuse, etc.) it short-circuits.
    fc::cache::Database::initialize();

    // Snapshot the existing dictionary BEFORE training overwrites it.
    // Recompress trains a fresh dict — but the bodies on disk were
    // compressed with the old one, so we need both: old for decompress,
    // new for recompress. Without this snapshot the old dict gets
    // clobbered by saveDictionary, every decompress hits "Dictionary
    // mismatch", and the worker silently writes empty bodies.
    const QByteArray oldDict =
        fc::cache::MessageRepository::dictionaryFor(accountId_);

    // 1. Sample bodies for training. ORDER BY RANDOM() across the
    //    whole table is O(N log N) but runs once and is bounded by
    //    LIMIT; on cache sizes of 100k messages it's milliseconds.
    qInfo("BodyCompression: accountId='%s' mode=%s — sampling",
          qUtf8Printable(accountId_),
          mode_ == Mode::Recompress ? "Recompress" : "InitialTrain");

    std::vector<QByteArray> samples;
    qint64 sampleBytesTotal = 0;
    {
        auto db = databaseHandle();
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT body_text FROM messages "
            "WHERE account_id = :a "
            "  AND body_text IS NOT NULL "
            "  AND length(body_text) > 0 "
            "ORDER BY RANDOM() LIMIT :n"));
        q.bindValue(QStringLiteral(":a"), accountId_);
        q.bindValue(QStringLiteral(":n"), kTrainingSampleCount);
        if (!q.exec()) {
            emit failed(accountId_,
                QStringLiteral("compression sample query: %1")
                  .arg(q.lastError().text()));
            thread_->quit();
            return;
        }
        samples.reserve(kTrainingSampleCount);
        while (q.next()) {
            QByteArray b = utf8OrBytes(q.value(0));
            // If a row is already compressed (Recompress mode), use
            // the snapshotted oldDict — saveDictionary will replace
            // the on-disk dict shortly, but these bytes were written
            // against the old one.
            if (fc::util::BodyCodec::isCompressed(b)) {
                if (oldDict.isEmpty()) {
                    emit failed(accountId_, QStringLiteral(
                        "compression: encountered compressed body but no "
                        "existing dictionary — refusing to wipe bodies."));
                    thread_->quit();
                    return;
                }
                const QByteArray decoded =
                    fc::util::BodyCodec::decompress(b, oldDict);
                if (decoded.isEmpty()) {
                    emit failed(accountId_, QStringLiteral(
                        "compression: decompress failed during sampling "
                        "(dictionary mismatch). Aborting to preserve bodies."));
                    thread_->quit();
                    return;
                }
                b = decoded;
            }
            if (b.size() > kTrainingPerSampleCap) {
                b = b.left(kTrainingPerSampleCap);
            }
            sampleBytesTotal += b.size();
            samples.push_back(std::move(b));
        }
    }
    if (samples.empty()) {
        emit failed(accountId_,
            QStringLiteral("compression: no bodies available to train on"));
        thread_->quit();
        return;
    }
    qInfo("BodyCompression: trained on %lld samples (%lld bytes total)",
          static_cast<long long>(samples.size()),
          static_cast<long long>(sampleBytesTotal));

    // 2. Train + persist the dictionary.
    const QByteArray dict = fc::util::BodyCodec::trainDictionary(
        samples, kDictSizeBytes);
    if (dict.isEmpty()) {
        emit failed(accountId_, QStringLiteral(
            "compression: dictionary training returned no data "
            "(insufficient distinct samples?)"));
        thread_->quit();
        return;
    }
    // Capture the count before freeing — saveDictionary needs the
    // pre-clear size for its sample_count telemetry column. The
    // previous order had us writing 0 to that column on every run.
    const int sampleCountForSave = static_cast<int>(samples.size());
    samples.clear();   // free the training corpus before backfill
    samples.shrink_to_fit();

    MessageRepository::saveDictionary(accountId_, dict,
        sampleCountForSave, sampleBytesTotal);
    // saveDictionary refreshes the in-memory cache for this account.

    // 3. Backfill. Walk rows in chunked transactions. Filter mode-
    //    appropriately: InitialTrain only touches plaintext rows;
    //    Recompress rewrites everything (in case the new dict is
    //    different from the old one).
    int totalToRewrite = 0;
    {
        auto db = databaseHandle();
        QSqlQuery q(db);
        const QString whereCompression =
            (mode_ == Mode::Recompress)
            ? QString()   // every row
            : QStringLiteral(" AND body_compression = 0");
        q.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM messages "
            "WHERE account_id = :a "
            "  AND ((body_text  IS NOT NULL AND length(body_text)  > 0) "
            "    OR (body_html  IS NOT NULL AND length(body_html)  > 0))"
            "%1").arg(whereCompression));
        q.bindValue(QStringLiteral(":a"), accountId_);
        if (q.exec() && q.next()) {
            totalToRewrite = q.value(0).toInt();
        }
    }
    qInfo("BodyCompression: %d rows to rewrite", totalToRewrite);
    emit progress(accountId_, 0, totalToRewrite);

    int done = 0;
    int failedUpdates = 0;
    qint64 savedBytes = 0;
    // Cursor-style pagination: each chunk SELECT pulls ids strictly
    // greater than the previous chunk's max id. Without this, a row
    // whose UPDATE failed mid-walk (e.g. transient SQLITE_BUSY) would
    // stay eligible for selection and reappear in every subsequent
    // chunk — infinite loop, `done` climbing past totalToRewrite.
    QString cursorId;   // empty = start at the beginning of id order
    while (true) {
        std::vector<QString> rowIds;
        {
            auto db = databaseHandle();
            QSqlQuery q(db);
            const QString whereCompression =
                (mode_ == Mode::Recompress)
                ? QString()
                : QStringLiteral(" AND body_compression = 0");
            // COALESCE(:cursor, '') because Qt's SQLite driver binds
            // an empty QString as SQL NULL — without the coalesce,
            // `id > NULL` is NULL (falsy) and the very first chunk
            // returns zero rows. With the coalesce, an empty cursor
            // becomes '', which compares less than any real (non-
            // empty) Gmail message id.
            q.prepare(QStringLiteral(
                "SELECT id FROM messages "
                "WHERE account_id = :a "
                "  AND id > COALESCE(:cursor, '') "
                "  AND ((body_text  IS NOT NULL AND length(body_text)  > 0) "
                "    OR (body_html  IS NOT NULL AND length(body_html)  > 0))"
                "%1 ORDER BY id LIMIT :n").arg(whereCompression));
            q.bindValue(QStringLiteral(":a"),      accountId_);
            q.bindValue(QStringLiteral(":cursor"), cursorId);
            q.bindValue(QStringLiteral(":n"),      kBackfillChunkSize);
            if (!q.exec()) {
                emit failed(accountId_,
                    QStringLiteral("compression chunk query: %1")
                      .arg(q.lastError().text()));
                thread_->quit();
                return;
            }
            while (q.next()) rowIds.push_back(q.value(0).toString());
        }
        if (rowIds.empty()) break;
        // Advance the cursor to the last id in this chunk, regardless
        // of whether individual UPDATEs succeed. Failed rows are
        // logged but never re-attempted by this run.
        cursorId = rowIds.back();

        // Rewrite the chunk inside one transaction. Each UPDATE is
        // a separate prepared statement; SQLite buffers them in the
        // WAL until commit.
        auto db = databaseHandle();
        if (!db.transaction()) {
            emit failed(accountId_,
                QStringLiteral("compression: BEGIN failed: %1")
                  .arg(db.lastError().text()));
            thread_->quit();
            return;
        }
        for (const auto& id : rowIds) {
            QSqlQuery sel(db);
            sel.prepare(QStringLiteral(
                "SELECT body_text, body_html, body_compression "
                "FROM messages WHERE account_id = :a AND id = :id"));
            sel.bindValue(QStringLiteral(":a"),  accountId_);
            sel.bindValue(QStringLiteral(":id"), id);
            if (!sel.exec() || !sel.next()) continue;

            QByteArray bt = utf8OrBytes(sel.value(0));
            QByteArray bh = utf8OrBytes(sel.value(1));
            const int existingFlag = sel.value(2).toInt();

            const qint64 beforeBytes = bt.size() + bh.size();

            // Decompress with the OLD dict snapshot first (the
            // typical case), falling back to the just-trained dict
            // for rows written by a concurrent sync. If BOTH fail
            // the row is an orphan from a prior aborted recompress
            // — the bytes on disk were compressed with a dict
            // neither we nor the DB has anymore. Skip it (don't
            // rewrite) and let the reader's next attempt re-fetch
            // from Gmail. One bad row no longer aborts the whole
            // recompress.
            bool rowSkipped = false;
            auto decompressOrSkip = [&](QByteArray& field,
                                          const char* which) -> bool {
                if (!fc::util::BodyCodec::isCompressed(field)) return true;
                if (!oldDict.isEmpty()) {
                    QByteArray decoded =
                        fc::util::BodyCodec::decompress(field, oldDict);
                    if (!decoded.isEmpty()) {
                        field = std::move(decoded);
                        return true;
                    }
                }
                QByteArray decoded =
                    fc::util::BodyCodec::decompress(field, dict);
                if (!decoded.isEmpty()) {
                    field = std::move(decoded);
                    return true;
                }
                qWarning("BodyCompression: %s decompress failed for "
                         "%s (orphaned dict); skipping row.",
                         which, qUtf8Printable(id));
                rowSkipped = true;
                return false;
            };
            if (!decompressOrSkip(bt, "body_text")
                || !decompressOrSkip(bh, "body_html")) {
                ++failedUpdates;
                continue;
            }
            Q_UNUSED(rowSkipped);

            const QByteArray newBt = bt.isEmpty()
                ? QByteArray() : fc::util::BodyCodec::compress(bt, dict);
            const QByteArray newBh = bh.isEmpty()
                ? QByteArray() : fc::util::BodyCodec::compress(bh, dict);
            const int newFlag =
                (fc::util::BodyCodec::isCompressed(newBt)
                 || fc::util::BodyCodec::isCompressed(newBh)) ? 1 : 0;

            QSqlQuery upd(db);
            upd.prepare(QStringLiteral(
                "UPDATE messages SET "
                "  body_text = :bt, body_html = :bh, "
                "  body_compression = :flag, "
                "  bytes_cached = :bc "
                "WHERE account_id = :a AND id = :id"));
            upd.bindValue(QStringLiteral(":bt"),   newBt);
            upd.bindValue(QStringLiteral(":bh"),   newBh);
            upd.bindValue(QStringLiteral(":flag"), newFlag);
            upd.bindValue(QStringLiteral(":bc"),   newBt.size() + newBh.size());
            upd.bindValue(QStringLiteral(":a"),    accountId_);
            upd.bindValue(QStringLiteral(":id"),   id);
            if (!upd.exec()) {
                qWarning("BodyCompression: update %s failed: %s",
                         qUtf8Printable(id),
                         qUtf8Printable(upd.lastError().text()));
                ++failedUpdates;
                continue;
            }

            savedBytes += beforeBytes - (newBt.size() + newBh.size());
            ++done;
            Q_UNUSED(existingFlag);
        }
        if (!db.commit()) {
            qWarning("BodyCompression: chunk COMMIT failed: %s",
                     qUtf8Printable(db.lastError().text()));
            db.rollback();
        }
        emit progress(accountId_, done, totalToRewrite);
    }

    // 4. VACUUM reclaims pages the rewritten BLOBs no longer use.
    //    VACUUM is itself memory-light (single-pass) but writes a
    //    full copy of the database; on cache sizes of a few GB this
    //    is the longest single step.
    {
        auto db = databaseHandle();
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("VACUUM"))) {
            qWarning("BodyCompression: VACUUM failed: %s",
                     qUtf8Printable(q.lastError().text()));
        }
    }

    qInfo("BodyCompression: done. rewroteCount=%d failedUpdates=%d "
          "savedBytes=%lld",
          done, failedUpdates, static_cast<long long>(savedBytes));
    emit finished(accountId_, done, savedBytes);
    thread_->quit();
}

}  // namespace fc::cache
