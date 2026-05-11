#include "cache/Migrations.h"

#include <QCoreApplication>
#include <QDir>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class TestMigrations : public QObject {
    Q_OBJECT
private:
    QTemporaryDir tmp_;

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        // Redirect QStandardPaths so the on-disk db lands in the tmp dir
        // (Database.cpp uses fc::util::cacheDbPath under the hood, but
        // these tests open their own connection by name to test Migrations).
        QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
        QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
        QStandardPaths::setTestModeEnabled(true);
    }

    void runsAllMigrationsAndIsIdempotent() {
        const QString conn  = QStringLiteral("test_migrations_conn");
        const QString path  = tmp_.filePath(QStringLiteral("c.db"));

        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(path);
            QVERIFY(db.open());

            fc::cache::Migrations::run(db);

            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral(
                "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'messages'")));
            QVERIFY(q.next());

            QVERIFY(q.exec(QStringLiteral(
                "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'messages_fts'")));
            QVERIFY(q.next());

            QVERIFY(q.exec(QStringLiteral(
                "SELECT value FROM meta WHERE key = 'schema_version'")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 8);   // bump on each schema change

            // body_html column landed in v3.
            QVERIFY(q.exec(QStringLiteral(
                "SELECT body_html FROM messages LIMIT 1")));   // shouldn't error
            // send_at landed in v4 (scheduled send).
            QVERIFY(q.exec(QStringLiteral(
                "SELECT send_at FROM outbox LIMIT 1")));
            // snooze_until landed in v5.
            QVERIFY(q.exec(QStringLiteral(
                "SELECT snooze_until FROM messages LIMIT 1")));

            // v6 multi-account: every per-account table now carries an
            // `account_id` column.
            QVERIFY(q.exec(QStringLiteral(
                "SELECT account_id FROM messages LIMIT 1")));
            QVERIFY(q.exec(QStringLiteral(
                "SELECT account_id FROM threads LIMIT 1")));
            QVERIFY(q.exec(QStringLiteral(
                "SELECT account_id FROM labels LIMIT 1")));
            QVERIFY(q.exec(QStringLiteral(
                "SELECT account_id FROM message_labels LIMIT 1")));
            QVERIFY(q.exec(QStringLiteral(
                "SELECT account_id FROM attachments LIMIT 1")));
            QVERIFY(q.exec(QStringLiteral(
                "SELECT account_id FROM drafts LIMIT 1")));
            QVERIFY(q.exec(QStringLiteral(
                "SELECT account_id FROM outbox LIMIT 1")));
            QVERIFY(q.exec(QStringLiteral(
                "SELECT account_id FROM pending_ops LIMIT 1")));

            // The accounts table exists and is empty on a fresh install
            // (the v0→v6 legacy seed migration was removed).
            QVERIFY(q.exec(QStringLiteral(
                "SELECT COUNT(*) FROM accounts")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 0);

            // account_meta exists and is empty.
            QVERIFY(q.exec(QStringLiteral(
                "SELECT COUNT(*) FROM account_meta")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 0);

            // Idempotency: second run should not fail.
            fc::cache::Migrations::run(db);
        }
        QSqlDatabase::removeDatabase(conn);
    }
};

QTEST_MAIN(TestMigrations)
#include "test_migrations.moc"
