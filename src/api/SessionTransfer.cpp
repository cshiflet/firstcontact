#include "SessionTransfer.h"

namespace fc::api {

SessionTransfer& SessionTransfer::instance() {
    static SessionTransfer inst;
    return inst;
}

void SessionTransfer::record(qint64 inBytes, qint64 outBytes) {
    // Add a small fixed allowance per request for HTTP headers we don't
    // measure exactly. ~256 B per direction is a reasonable mid-point
    // for Gmail's typical Authorization + Accept + standard server
    // headers; dwarfed by message bodies anyway.
    constexpr qint64 kHeaderEstimate = 256;
    bytesIn_.fetch_add(inBytes  + kHeaderEstimate, std::memory_order_relaxed);
    bytesOut_.fetch_add(outBytes + kHeaderEstimate, std::memory_order_relaxed);
    requests_.fetch_add(1, std::memory_order_relaxed);
    emit changed();
}

void SessionTransfer::reset() {
    bytesIn_.store(0,  std::memory_order_relaxed);
    bytesOut_.store(0, std::memory_order_relaxed);
    requests_.store(0, std::memory_order_relaxed);
    emit changed();
}

}  // namespace fc::api
