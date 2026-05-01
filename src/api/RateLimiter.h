#pragma once

#include <QtGlobal>

namespace fc::api {

// Computes the next backoff delay for a sequence of failed attempts.
// Jittered exponential backoff capped at 60s, max 6 attempts.
//
// Pure stateless math — call sites pass the current attempt count.
class RateLimiter {
public:
    static constexpr int    kMaxAttempts   = 6;
    static constexpr qint64 kBaseDelayMs   = 500;
    static constexpr qint64 kCapMs         = 60'000;

    // Returns delay in ms for the given attempt (1-based). 0 ⇒ stop retrying.
    static qint64 nextDelayMs(int attempt, int retryAfterSec = 0);
};

}  // namespace fc::api
