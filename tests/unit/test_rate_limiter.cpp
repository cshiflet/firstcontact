#include "api/RateLimiter.h"

#include <QObject>
#include <QtTest>

class TestRateLimiter : public QObject {
    Q_OBJECT
private slots:
    void honorsRetryAfter() {
        QCOMPARE(fc::api::RateLimiter::nextDelayMs(1, /*retryAfter=*/3), 3000);
    }

    void capsAtCeiling() {
        QVERIFY(fc::api::RateLimiter::nextDelayMs(20, 0) == 0);  // beyond max
    }

    void monotonicallyIncreasing() {
        const auto a = fc::api::RateLimiter::nextDelayMs(1);
        const auto b = fc::api::RateLimiter::nextDelayMs(3);
        QVERIFY(a > 0);
        QVERIFY(b >= a);          // jittered, so >=
        QVERIFY(b <= fc::api::RateLimiter::kCapMs);
    }
};

QTEST_APPLESS_MAIN(TestRateLimiter)
#include "test_rate_limiter.moc"
