#include "RateLimiter.h"

#include <QRandomGenerator>

#include <algorithm>

namespace fc::api {

qint64 RateLimiter::nextDelayMs(int attempt, int retryAfterSec) {
    if (attempt > kMaxAttempts) return 0;

    if (retryAfterSec > 0) {
        return std::min<qint64>(kCapMs, qint64(retryAfterSec) * 1000);
    }

    qint64 delay = kBaseDelayMs;
    for (int i = 1; i < attempt; ++i) delay *= 2;
    delay = std::min<qint64>(kCapMs, delay);

    // Full jitter: pick uniformly in [delay/2, delay].
    const qint64 half = delay / 2;
    const qint64 jitter = QRandomGenerator::global()->bounded(qint32(delay - half + 1));
    return half + jitter;
}

}  // namespace fc::api
