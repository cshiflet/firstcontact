#pragma once

#include <QObject>
#include <QString>

class QThread;

namespace fc::cache {

// Background task: trains a zstd dictionary from a sample of an
// account's bodies, saves it to body_compression_dict, and rewrites
// every uncompressed body_text / body_html row through the new
// codec. Memory- and disk-frugal: processes rows in chunked
// transactions (small WAL footprint, predictable peak RAM) and runs
// VACUUM at the end to reclaim the old plaintext bytes.
//
// Owns its own QThread; safe to start() from the UI thread. Caller
// connects to the progress/finished/failed signals. The worker
// deletes itself once finished/failed has fired and the thread has
// stopped, so callers should hold a QPointer.
class BodyCompressionWorker : public QObject {
    Q_OBJECT
public:
    enum class Mode {
        // First-time training: only touches rows where
        // body_compression = 0. Existing compressed rows (if any
        // from a previous run) are left alone.
        InitialTrain,

        // Full recompression: invalidates the cached dict, retrains
        // from scratch, then rewrites every body row regardless of
        // current compression state. Used by Settings → "Recompress".
        Recompress,
    };

    explicit BodyCompressionWorker(const QString& accountId, Mode mode,
                                    QObject* parent = nullptr);
    ~BodyCompressionWorker() override;

    // Kick off. Must be called exactly once. Posts the actual work
    // onto the worker thread.
    void start();

signals:
    void progress(const QString& accountId, int done, int total);
    void finished(const QString& accountId,
                   int rewroteCount, qint64 savedBytes);
    void failed(const QString& accountId, const QString& reason);

private slots:
    // Runs on the worker thread once via QueuedConnection.
    void doWork();

private:
    QString  accountId_;
    Mode     mode_;
    QThread* thread_ = nullptr;
};

}  // namespace fc::cache
