// Smoke test for SyncService's background-crawl bookkeeping:
//   - resetBackgroundCrawlProgress wipes only the labelCrawl/*/exhausted
//     rows out of account_meta — it must not touch other meta keys.
//   - The crawler reads its "skip exhausted" flag from the same key
//     shape the reset clears (labelCrawl/<id>/exhausted = "1").
//
// We don't drive SyncService's QNAM in unit tests; what we exercise
// here is the meta-key bookkeeping that the crawler relies on. The
// "pick next label" round-trip logic is covered by integration when
// the crawler runs end-to-end against a real Gmail account.

#include "cache/Database.h"
#include "cache/MetaRepository.h"
#include "cache/Migrations.h"

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace fc::cache { QSqlDatabase databaseHandle(); }

namespace {

constexpr const char* kAcc =
    "00000000-0000-4000-8000-cccccccccccc";

QString acc() { return QString::fromLatin1(kAcc); }

void seedAccount() {
    auto db = fc::cache::databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO accounts(id, email, sort_order, created_at) "
        "VALUES(:id, :em, 0, strftime('%s','now')*1000)"));
    q.bindValue(QStringLiteral(":id"), acc());
    q.bindValue(QStringLiteral(":em"),
                QStringLiteral("crawler-test@example.test"));
    q.exec();
}

// Directly run the same DELETE the production resetBackgroundCrawlProgress
// uses. Inlined here because SyncService can't be constructed standalone
// (it wants a GmailClient, which wants a RestClient + auth).
void resetCrawlMeta(const QString& accountId) {
    auto db = fc::cache::databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "DELETE FROM account_meta "
        "WHERE account_id = :a AND key LIKE 'labelCrawl/%/exhausted'"));
    q.bindValue(QStringLiteral(":a"), accountId);
    q.exec();
}

QString crawlExhaustedKey(const QString& labelId) {
    return QStringLiteral("labelCrawl/%1/exhausted").arg(labelId);
}

}  // namespace

class TestBackgroundCrawler : public QObject {
    Q_OBJECT
private:
    QTemporaryDir tmp_;

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
        QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
        QStandardPaths::setTestModeEnabled(true);
        const QString dbPath = fc::cache::Database::filePath();
        QFile::remove(dbPath);
        QFile::remove(dbPath + QStringLiteral("-wal"));
        QFile::remove(dbPath + QStringLiteral("-shm"));
        fc::cache::Database::initialize();
        seedAccount();
    }

    // Setting / reading the per-label exhausted flag survives a
    // round-trip through account_meta.
    void exhaustedFlagRoundTrips() {
        fc::cache::MetaRepository::set(acc(),
            crawlExhaustedKey(QStringLiteral("INBOX")),
            QStringLiteral("1"));
        QCOMPARE(fc::cache::MetaRepository::get(acc(),
                    crawlExhaustedKey(QStringLiteral("INBOX"))),
                 QStringLiteral("1"));
    }

    // resetBackgroundCrawlProgress must drop ONLY the labelCrawl/*/exhausted
    // rows. Other meta keys (history_id, email, …) survive.
    void resetClearsOnlyCrawlFlags() {
        // Seed: one crawl flag + one foreign meta key.
        fc::cache::MetaRepository::set(acc(),
            crawlExhaustedKey(QStringLiteral("STARRED")),
            QStringLiteral("1"));
        fc::cache::MetaRepository::set(acc(),
            crawlExhaustedKey(QStringLiteral("SENT")),
            QStringLiteral("1"));
        fc::cache::MetaRepository::setHistoryId(acc(),
            QStringLiteral("hid-12345"));
        fc::cache::MetaRepository::set(acc(),
            QStringLiteral("email"),
            QStringLiteral("crawler-test@example.test"));

        resetCrawlMeta(acc());

        QVERIFY(fc::cache::MetaRepository::get(acc(),
                    crawlExhaustedKey(QStringLiteral("STARRED"))).isEmpty());
        QVERIFY(fc::cache::MetaRepository::get(acc(),
                    crawlExhaustedKey(QStringLiteral("SENT"))).isEmpty());
        QCOMPARE(fc::cache::MetaRepository::historyId(acc()),
                 QStringLiteral("hid-12345"));
        QCOMPARE(fc::cache::MetaRepository::get(acc(),
                    QStringLiteral("email")),
                 QStringLiteral("crawler-test@example.test"));
    }

    // Reset is per-account: clearing account A's crawl flags must not
    // touch account B's.
    void resetIsScopedToAccount() {
        const QString accB =
            QStringLiteral("00000000-0000-4000-8000-dddddddddddd");
        auto db = fc::cache::databaseHandle();
        QSqlQuery seed(db);
        seed.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO accounts(id, email, sort_order, created_at) "
            "VALUES(:id, :em, 1, strftime('%s','now')*1000)"));
        seed.bindValue(QStringLiteral(":id"), accB);
        seed.bindValue(QStringLiteral(":em"),
                       QStringLiteral("crawler-test-b@example.test"));
        QVERIFY(seed.exec());

        fc::cache::MetaRepository::set(acc(),
            crawlExhaustedKey(QStringLiteral("INBOX")),
            QStringLiteral("1"));
        fc::cache::MetaRepository::set(accB,
            crawlExhaustedKey(QStringLiteral("INBOX")),
            QStringLiteral("1"));

        resetCrawlMeta(acc());

        QVERIFY(fc::cache::MetaRepository::get(acc(),
                    crawlExhaustedKey(QStringLiteral("INBOX"))).isEmpty());
        QCOMPARE(fc::cache::MetaRepository::get(accB,
                    crawlExhaustedKey(QStringLiteral("INBOX"))),
                 QStringLiteral("1"));
    }
};

QTEST_MAIN(TestBackgroundCrawler)
#include "test_background_crawler.moc"
