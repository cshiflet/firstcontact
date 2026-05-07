#pragma once

#include <QObject>

#include <atomic>

namespace fc::api {

// Lightweight counter for bytes-on-the-wire to / from Gmail since the
// process started. Bumped by RestClient on every successful or failed
// response; surfaced by MainWindow as a "session transfer" tooltip on
// the status bar so users on metered or slow links can see what the
// app is actually costing them.
//
// Lock-free atomic counters — RestClient lives on the sync thread,
// readers live on the UI thread, both touch the same instance. The
// `changed()` signal hops via the default Auto/Queued connection so
// the receiver runs on the UI thread.
class SessionTransfer : public QObject {
    Q_OBJECT
public:
    static SessionTransfer& instance();

    qint64 bytesIn() const  { return bytesIn_.load(std::memory_order_relaxed); }
    qint64 bytesOut() const { return bytesOut_.load(std::memory_order_relaxed); }
    int    requestCount() const {
        return requests_.load(std::memory_order_relaxed);
    }

    // Called from RestClient. inBytes / outBytes are the wire sizes of
    // the response body and request body respectively; we don't bother
    // counting headers individually (estimate ~256 B per side, added to
    // the totals so the tooltip isn't comically optimistic).
    void record(qint64 inBytes, qint64 outBytes);

    // Resets the counters to zero — used by the status-bar widget's
    // right-click "Reset" item if we ever add one. For now no caller.
    void reset();

signals:
    void changed();

private:
    SessionTransfer() = default;

    std::atomic<qint64> bytesIn_{0};
    std::atomic<qint64> bytesOut_{0};
    std::atomic<int>    requests_{0};
};

}  // namespace fc::api
