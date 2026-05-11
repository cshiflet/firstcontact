#include "cache/Database.h"
#include "cache/DraftRepository.h"

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtTest>

namespace fc::cache { QSqlDatabase databaseHandle(); }

class TestDraftRepository : public QObject {
    Q_OBJECT
private:
    // Multi-account schema requires every per-account row's account_id to
    // resolve to an accounts row. Tests seed one explicitly — the legacy
    // synthetic-seed migration was removed, so a fresh DB has no rows.
    static constexpr const char* kAccountId =
        "00000000-0000-4000-8000-aaaaaaaaaaa1";

    static void seedAccount() {
        auto db = fc::cache::databaseHandle();
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO accounts(id, email, sort_order, created_at) "
            "VALUES(:id, :em, 0, strftime('%s','now')*1000)"));
        q.bindValue(QStringLiteral(":id"), QString::fromLatin1(kAccountId));
        q.bindValue(QStringLiteral(":em"),
                    QStringLiteral("draft-test@example.test"));
        QVERIFY(q.exec());
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
        QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
        // Wipe any prior state from a previous run.
        QFile::remove(fc::cache::Database::filePath());
        fc::cache::Database::initialize();
        seedAccount();
    }

    void upsertAssignsLocalIdAndPersists() {
        const QString account = QString::fromLatin1(kAccountId);
        fc::cache::DraftRow d;
        d.subject  = QStringLiteral("hello");
        d.toAddrs  = {QStringLiteral("a@x.test")};
        d.bodyText = QStringLiteral("body");
        d.dirty    = true;

        const QString id = fc::cache::DraftRepository::upsert(account, d);
        QVERIFY(!id.isEmpty());
        QVERIFY(id.startsWith(QStringLiteral("tmp-")));

        const auto loaded = fc::cache::DraftRepository::byId(account, id);
        QCOMPARE(loaded.subject,  QStringLiteral("hello"));
        QCOMPARE(loaded.toAddrs.size(), 1);
        QCOMPARE(loaded.toAddrs.front(), QStringLiteral("a@x.test"));
        QCOMPARE(loaded.bodyText, QStringLiteral("body"));
        QVERIFY(loaded.dirty);
    }

    void dirtyDraftsListsOnlyDirty() {
        const QString account = QString::fromLatin1(kAccountId);
        fc::cache::DraftRow clean;
        clean.subject = QStringLiteral("clean");
        clean.dirty   = false;
        const QString cleanId = fc::cache::DraftRepository::upsert(account, clean);

        fc::cache::DraftRow dirty;
        dirty.subject = QStringLiteral("dirty");
        dirty.dirty   = true;
        const QString dirtyId = fc::cache::DraftRepository::upsert(account, dirty);

        const auto dirties = fc::cache::DraftRepository::dirtyDrafts(account);
        bool foundDirty = false, foundClean = false;
        for (const auto& d : dirties) {
            if (d.id == dirtyId) foundDirty = true;
            if (d.id == cleanId) foundClean = true;
        }
        QVERIFY(foundDirty);
        QVERIFY(!foundClean);
    }

    void markSyncedRekeysToGmailDraftId() {
        const QString account = QString::fromLatin1(kAccountId);
        fc::cache::DraftRow d;
        d.subject = QStringLiteral("sync me");
        d.dirty   = true;
        const QString localId  = fc::cache::DraftRepository::upsert(account, d);
        const QString remoteId = QStringLiteral("draft-abc-123");

        fc::cache::DraftRepository::markSynced(account, localId, remoteId);

        QVERIFY(fc::cache::DraftRepository::byId(account, localId).id.isEmpty());
        const auto reloaded = fc::cache::DraftRepository::byId(account, remoteId);
        QCOMPARE(reloaded.id, remoteId);
        QVERIFY(!reloaded.dirty);
    }
};

QTEST_MAIN(TestDraftRepository)
#include "test_draft_repository.moc"
