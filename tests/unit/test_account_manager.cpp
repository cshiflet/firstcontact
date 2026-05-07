// AccountManager: in-memory view of the accounts table + active selection.

#include "account/AccountManager.h"
#include "cache/Database.h"
#include "cache/MetaRepository.h"
#include "cache/PendingOpsRepository.h"

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
};

QTEST_MAIN(TestAccountManager)
#include "test_account_manager.moc"
