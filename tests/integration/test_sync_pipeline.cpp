// Replays a Gmail messages.get response through the full
// MessageParser → MessageRepository → listByLabel pipeline against a
// real SQLite cache (temp directory, fresh schema). This is the
// "does the data layer round-trip correctly?" smoke test.
//
// Focused on the parser+repo seam, not on SyncService's network/FSM
// — those have unit-test surfaces (the FSM races and lifecycle were
// covered in the round-1..4 review). This test catches schema drift
// between MessageParser's output Message shape and the cache columns.

#include "api/MessageParser.h"
#include "cache/Database.h"
#include "cache/MessageRepository.h"
#include "models/Message.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace fc::cache { QSqlDatabase databaseHandle(); }

namespace {

// Production seeds the labels table from labels.list during InitialSync.
// Tests don't run InitialSync, so writeLabelEdges' WHERE EXISTS guard
// would otherwise drop every label edge. Insert the system labels we
// reference in the fixtures.
void seedSystemLabels() {
    auto db = fc::cache::databaseHandle();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO labels(id, name, type) VALUES(:id, :id, 'system')"));
    for (const QString& id : {QStringLiteral("INBOX"),
                               QStringLiteral("UNREAD"),
                               QStringLiteral("STARRED"),
                               QStringLiteral("CATEGORY_PERSONAL")}) {
        q.bindValue(QStringLiteral(":id"), id);
        q.exec();
    }
}

}  // namespace

namespace {

// One concrete Gmail messages.get?format=metadata response. Real wire
// shape, condensed to the fields MessageParser actually reads:
// id, threadId, labelIds, snippet, internalDate, payload.headers.
constexpr const char* kFixture = R"json({
  "id": "msg-aaa",
  "threadId": "thr-1",
  "labelIds": ["INBOX", "UNREAD", "CATEGORY_PERSONAL"],
  "snippet": "Hello from a fixture.",
  "internalDate": "1700000000000",
  "payload": {
    "headers": [
      { "name": "From",    "value": "Alice <alice@example.com>" },
      { "name": "To",      "value": "bob@example.com" },
      { "name": "Subject", "value": "Fixture subject one" },
      { "name": "Date",    "value": "Tue, 14 Nov 2023 22:13:20 GMT" },
      { "name": "Message-ID", "value": "<msg-aaa@example.com>" }
    ],
    "mimeType": "text/plain",
    "body": { "size": 12 }
  }
})json";

constexpr const char* kFixture2 = R"json({
  "id": "msg-bbb",
  "threadId": "thr-2",
  "labelIds": ["INBOX", "STARRED"],
  "snippet": "Older message.",
  "internalDate": "1690000000000",
  "payload": {
    "headers": [
      { "name": "From",    "value": "Carol <carol@example.com>" },
      { "name": "Subject", "value": "Older one" }
    ]
  }
})json";

}  // namespace

class TestSyncPipeline : public QObject {
    Q_OBJECT

private:
    QTemporaryDir tmp_;

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        // Redirect QStandardPaths so fc::util::cacheDbPath lands in a
        // /tmp/qttest-* tree. setTestModeEnabled redirects but does NOT
        // wipe — successive runs of this binary would otherwise reuse a
        // stale DB and fail with phantom rows. Delete the cache file
        // explicitly before Database::initialize() opens its connection.
        QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
        QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
        QStandardPaths::setTestModeEnabled(true);
        const QString dbPath = fc::cache::Database::filePath();
        QFile::remove(dbPath);
        QFile::remove(dbPath + QStringLiteral("-wal"));
        QFile::remove(dbPath + QStringLiteral("-shm"));
        fc::cache::Database::initialize();
    }

    // The bedrock case: parse one Gmail message, upsert it, list it back
    // by label, verify the headers / labels round-tripped.
    void parsesAndStoresOneMessage() {
        const auto doc = QJsonDocument::fromJson(QByteArray(kFixture));
        QVERIFY(doc.isObject());

        const auto m = fc::api::MessageParser::parse(doc.object());
        QCOMPARE(m.id, QStringLiteral("msg-aaa"));
        QCOMPARE(m.threadId, QStringLiteral("thr-1"));
        QCOMPARE(m.subject, QStringLiteral("Fixture subject one"));
        QCOMPARE(m.fromAddr, QStringLiteral("alice@example.com"));
        QVERIFY(m.labelIds.contains(QStringLiteral("INBOX")));
        QVERIFY(m.labelIds.contains(QStringLiteral("UNREAD")));
        QVERIFY(m.isUnread);

        // Seed the labels table — writeLabelEdges() skips edges whose
        // label_id is not in the labels table (defends against unsynced
        // CHAT / cross-client labels). Real production seeds these via
        // labels.list during InitialSync; tests do it explicitly.
        seedSystemLabels();

        fc::cache::MessageRepository::upsert(m);

        const auto rows = fc::cache::MessageRepository::listByLabel(
            QStringLiteral("INBOX"), /*limit=*/10, /*offset=*/0);
        QCOMPARE(rows.size(), size_t(1));
        QCOMPARE(rows[0].id, QStringLiteral("msg-aaa"));
        QCOMPARE(rows[0].subject, QStringLiteral("Fixture subject one"));
        // isUnread is a derived column-level flag on messages, set by
        // MessageParser when "UNREAD" appears in labelIds. Survives the
        // round-trip even if the label edge is skipped (unseeded label).
        QVERIFY(rows[0].isUnread);
        // Edge hydration: now that the label is seeded, UNREAD shows up.
        QVERIFY(rows[0].labelIds.contains(QStringLiteral("UNREAD")));
        QVERIFY(rows[0].labelIds.contains(QStringLiteral("INBOX")));
    }

    // listByLabel's "ORDER BY internal_date DESC" contract — newer first.
    // Catches a regression where the order clause is dropped or inverted.
    void orderingIsNewestFirst() {
        const auto m1 = fc::api::MessageParser::parse(
            QJsonDocument::fromJson(QByteArray(kFixture2)).object());
        fc::cache::MessageRepository::upsert(m1);

        const auto rows = fc::cache::MessageRepository::listByLabel(
            QStringLiteral("INBOX"), /*limit=*/10, /*offset=*/0);
        QCOMPARE(rows.size(), size_t(2));
        // msg-aaa (1700000000000) > msg-bbb (1690000000000) → first row.
        QCOMPARE(rows[0].id, QStringLiteral("msg-aaa"));
        QCOMPARE(rows[1].id, QStringLiteral("msg-bbb"));
    }

    // unreadOnly=true should drop messages that don't carry UNREAD.
    // msg-aaa is unread; msg-bbb (STARRED but no UNREAD) should not appear.
    void unreadOnlyFilter() {
        const auto rows = fc::cache::MessageRepository::listByLabel(
            QStringLiteral("INBOX"), /*limit=*/10, /*offset=*/0,
            /*unreadOnly=*/true);
        QCOMPARE(rows.size(), size_t(1));
        QCOMPARE(rows[0].id, QStringLiteral("msg-aaa"));
    }

    // Upsert is idempotent — re-applying the same fixture must not
    // duplicate rows in either messages or message_labels.
    void upsertIsIdempotent() {
        const auto m = fc::api::MessageParser::parse(
            QJsonDocument::fromJson(QByteArray(kFixture)).object());
        fc::cache::MessageRepository::upsert(m);   // again
        fc::cache::MessageRepository::upsert(m);   // and again

        const auto rows = fc::cache::MessageRepository::listByLabel(
            QStringLiteral("INBOX"), /*limit=*/10, /*offset=*/0);
        // Still exactly the two messages from previous tests, no dupes.
        QCOMPARE(rows.size(), size_t(2));
    }
};

QTEST_MAIN(TestSyncPipeline)
#include "test_sync_pipeline.moc"
