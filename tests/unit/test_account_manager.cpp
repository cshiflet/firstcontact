// AccountManager: in-memory view of the accounts table + active selection.

#include "account/AccountManager.h"
#include "cache/Database.h"
#include "cache/LabelRepository.h"
#include "cache/MessageRepository.h"
#include "cache/MetaRepository.h"
#include "cache/Migrations.h"   // databaseHandle()
#include "cache/PendingOpsRepository.h"
#include "models/Message.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlQuery>

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>

class TestAccountManager : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
        QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
        QFile::remove(fc::cache::Database::filePath());
        fc::cache::Database::initialize();
    }

    void afterMigrationOnlyTheLegacySeedAccountIsPresent() {
        fc::account::AccountManager mgr;
        const auto list = mgr.accounts();
        QCOMPARE(int(list.size()), 1);
        QCOMPARE(list.front().id,
                 QStringLiteral("00000000-0000-4000-8000-000000000001"));
        QVERIFY(list.front().isDefault);
        // The legacy seed picks the COALESCE fallback because the test DB
        // had no meta.email row before migration ran.
        QCOMPARE(list.front().email, QStringLiteral("legacy@local"));
        // Default-account selection lands on the legacy id.
        QCOMPARE(mgr.currentAccountId(),
                 QStringLiteral("00000000-0000-4000-8000-000000000001"));
    }

    void addMintsAUuidAndPersists() {
        fc::account::AccountManager mgr;
        QSignalSpy changed(&mgr, &fc::account::AccountManager::accountsChanged);
        const QString id = mgr.add(QStringLiteral("alice@example.test"),
                                    QStringLiteral("Alice"));
        QVERIFY(!id.isEmpty());
        QVERIFY(changed.count() >= 1);

        const auto info = mgr.accountById(id);
        QCOMPARE(info.email,       QStringLiteral("alice@example.test"));
        QCOMPARE(info.displayName, QStringLiteral("Alice"));
        QVERIFY(!info.isDefault);   // legacy seed already holds default

        // Round-trip: new manager instance reads the same data.
        fc::account::AccountManager mgr2;
        bool found = false;
        for (const auto& a : mgr2.accounts()) {
            if (a.email == QStringLiteral("alice@example.test")) {
                found = true;
                QCOMPARE(a.id, id);
                break;
            }
        }
        QVERIFY(found);
    }

    void addIsIdempotentByEmail() {
        fc::account::AccountManager mgr;
        const QString first = mgr.add(QStringLiteral("bob@example.test"),
                                       QStringLiteral("Bob"));
        const QString second = mgr.add(QStringLiteral("bob@example.test"),
                                        QStringLiteral("Bobby"));
        QCOMPARE(second, first);   // same id reused
        QCOMPARE(mgr.accountById(first).displayName, QStringLiteral("Bobby"));
    }

    void setDefaultPromotesExactlyOneRow() {
        fc::account::AccountManager mgr;
        const QString cId = mgr.add(QStringLiteral("c@example.test"));
        QVERIFY(!cId.isEmpty());

        mgr.setDefault(cId);
        const auto list = mgr.accounts();
        int defaults = 0;
        for (const auto& a : list) if (a.isDefault) ++defaults;
        QCOMPARE(defaults, 1);
        QCOMPARE(mgr.accountById(cId).isDefault, true);
    }

    void setCurrentAccountIdEmitsAndStampsLastUsedAt() {
        fc::account::AccountManager mgr;
        const QString dId = mgr.add(QStringLiteral("d@example.test"));
        QVERIFY(!dId.isEmpty());

        QSignalSpy spy(&mgr, &fc::account::AccountManager::currentAccountChanged);
        mgr.setCurrentAccountId(dId);
        QCOMPARE(mgr.currentAccountId(), dId);
        QVERIFY(spy.count() >= 1);

        // Stamping last_used_at moves d ahead of the legacy seed in
        // the most-recently-used selection rule.
        QVERIFY(mgr.accountById(dId).lastUsedAt > 0);
    }

    void removeCascadesAndClearsCurrent() {
        fc::account::AccountManager mgr;
        const QString eId = mgr.add(QStringLiteral("e@example.test"));
        mgr.setCurrentAccountId(eId);
        QCOMPARE(mgr.currentAccountId(), eId);

        QVERIFY(mgr.remove(eId));
        // remove() picks a new current via selectInitialCurrent; it
        // must NOT leave the manager pointing at the deleted id.
        QVERIFY(mgr.currentAccountId() != eId);
        QVERIFY(mgr.accountById(eId).id.isEmpty());
    }

    void setCurrentAccountIdRejectsUnknownIds() {
        fc::account::AccountManager mgr;
        const QString prev = mgr.currentAccountId();
        QSignalSpy spy(&mgr, &fc::account::AccountManager::currentAccountChanged);
        mgr.setCurrentAccountId(QStringLiteral("not-a-real-id"));
        QCOMPARE(mgr.currentAccountId(), prev);
        QCOMPARE(spy.count(), 0);
    }

    void accentColorRoundTrips() {
        fc::account::AccountManager mgr;
        const QString id = mgr.add(QStringLiteral("ac@example.test"));
        QVERIFY(!id.isEmpty());

        // Palette has the eight known slugs.
        const auto palette = fc::account::AccountManager::accentPalette();
        QCOMPARE(palette.size(), 8);
        QVERIFY(palette.contains(QStringLiteral("blue")));

        // Each slug maps to a valid colour; an unknown slug returns
        // an invalid sentinel so the UI knows to fall back.
        QVERIFY(fc::account::AccountManager::accentColorFor(
            QStringLiteral("blue")).isValid());
        QVERIFY(!fc::account::AccountManager::accentColorFor(
            QStringLiteral("not-a-slug")).isValid());

        // Setting + reading round-trips through the cache.
        mgr.setAccentColor(id, QStringLiteral("teal"));
        QCOMPARE(mgr.accountById(id).colorHint, QStringLiteral("teal"));

        // Clearing (empty slug) writes NULL.
        mgr.setAccentColor(id, QString());
        QCOMPARE(mgr.accountById(id).colorHint, QString());
    }

    void dropCacheWipesPerAccountTablesButKeepsAccountsRow() {
        fc::account::AccountManager mgr;
        const QString id = mgr.add(QStringLiteral("dc@example.test"));
        QVERIFY(!id.isEmpty());

        // Seed something in account_meta and pending_ops to verify the
        // wipe touches the right tables.
        fc::cache::MetaRepository::set(id,
            QStringLiteral("history_id"), QStringLiteral("999"));
        fc::cache::PendingOpsRepository::enqueueModify(id,
            QStringLiteral("msg-x"),
            {QStringLiteral("STARRED")},
            {});

        QVERIFY(mgr.dropCache(id));

        // accounts row remains.
        QVERIFY(!mgr.accountById(id).id.isEmpty());

        // account_meta row is gone.
        QVERIFY(fc::cache::MetaRepository::historyId(id).isEmpty());

        // pending_ops row is gone.
        const auto due = fc::cache::PendingOpsRepository::due(id);
        QVERIFY(due.empty());
    }

    void cacheSizeReturnsBytesCachedSum() {
        fc::account::AccountManager mgr;
        const QString id = mgr.add(QStringLiteral("size@example.test"));
        QVERIFY(!id.isEmpty());

        // Seed a label so the message FK is satisfied.
        fc::cache::LabelRow lbl;
        lbl.accountId = id;
        lbl.id        = QStringLiteral("INBOX");
        lbl.name      = QStringLiteral("Inbox");
        lbl.type      = QStringLiteral("system");
        fc::cache::LabelRepository::upsert(id, lbl);

        // Insert a couple of messages with non-empty bodyText so
        // bytes_cached has a non-zero value.
        for (int i = 0; i < 3; ++i) {
            fc::Message m;
            m.accountId    = id;
            m.id           = QStringLiteral("msg-%1").arg(i);
            m.threadId     = QStringLiteral("thr-%1").arg(i);
            m.internalDate = 1700000000000LL;
            m.subject      = QStringLiteral("subject");
            m.bodyText     = QString(1024, QChar('x'));   // 1KB each
            m.labelIds     = {QStringLiteral("INBOX")};
            QVERIFY(fc::cache::MessageRepository::upsert(id, m) > 0);
        }

        const qint64 size = mgr.cacheSizeFor(id);
        QVERIFY(size >= 3 * 1024);
    }

    void orphanedAccountIdsListsRowsWithoutAccountsEntry() {
        fc::account::AccountManager mgr;

        // Insert a message with an account_id that has no accounts row.
        // We sidestep the FK chain by using account_meta (which carries
        // account_id but its FK cascades only on accounts deletion).
        // Easiest path: directly INSERT a row that bypasses the FK
        // (account_meta FK is ON DELETE CASCADE, but inserting an
        // account_id that doesn't exist also fails). So we add an
        // accounts row, write to account_meta, then DELETE the
        // accounts row out-of-band (cascades to account_meta — so the
        // simulated orphan can't actually be created via the public
        // API). The test relies on inserting into account_meta via
        // direct SQL after disabling FKs briefly.

        auto db = fc::cache::databaseHandle();
        QSqlQuery off(db);
        off.exec(QStringLiteral("PRAGMA foreign_keys = OFF"));
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO account_meta(account_id, key, value) "
            "VALUES('orphan-test-id', 'history_id', '42')"));
        QVERIFY(ins.exec());
        QSqlQuery on(db);
        on.exec(QStringLiteral("PRAGMA foreign_keys = ON"));

        const auto orphans = mgr.orphanedAccountIds();
        QVERIFY(orphans.contains(QStringLiteral("orphan-test-id")));

        const int dropped = mgr.dropOrphanedCache();
        QVERIFY(dropped >= 1);

        // Subsequent call should find nothing.
        QVERIFY(!mgr.orphanedAccountIds()
                    .contains(QStringLiteral("orphan-test-id")));
    }

    void clearMessagesOlderThanRespectsCutoff() {
        fc::account::AccountManager mgr;
        const QString id = mgr.add(QStringLiteral("aged@example.test"));
        QVERIFY(!id.isEmpty());

        // Seed INBOX so the FK is satisfied.
        fc::cache::LabelRow lbl;
        lbl.accountId = id;
        lbl.id        = QStringLiteral("INBOX");
        lbl.name      = QStringLiteral("Inbox");
        lbl.type      = QStringLiteral("system");
        fc::cache::LabelRepository::upsert(id, lbl);

        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        // Old message (45 days old).
        {
            fc::Message m;
            m.accountId    = id;
            m.id           = QStringLiteral("old-msg");
            m.threadId     = QStringLiteral("old-thr");
            m.internalDate = now - qint64(45) * 24 * 60 * 60 * 1000LL;
            m.subject      = QStringLiteral("old");
            m.bodyText     = QStringLiteral("old body");
            m.labelIds     = {QStringLiteral("INBOX")};
            fc::cache::MessageRepository::upsert(id, m);
        }
        // Fresh message (5 days old).
        {
            fc::Message m;
            m.accountId    = id;
            m.id           = QStringLiteral("fresh-msg");
            m.threadId     = QStringLiteral("fresh-thr");
            m.internalDate = now - qint64(5) * 24 * 60 * 60 * 1000LL;
            m.subject      = QStringLiteral("fresh");
            m.bodyText     = QStringLiteral("fresh body");
            m.labelIds     = {QStringLiteral("INBOX")};
            fc::cache::MessageRepository::upsert(id, m);
        }

        // Drop messages older than 30 days. The 45-day-old one goes;
        // the 5-day-old stays.
        const int n = mgr.clearMessagesOlderThan(id, 30);
        QCOMPARE(n, 1);

        QVERIFY(fc::cache::MessageRepository::byId(id,
            QStringLiteral("old-msg")).id.isEmpty());
        QCOMPARE(fc::cache::MessageRepository::byId(id,
            QStringLiteral("fresh-msg")).id, QStringLiteral("fresh-msg"));
    }
};

QTEST_MAIN(TestAccountManager)
#include "test_account_manager.moc"
