#include "cache/Database.h"
#include "cache/DraftRepository.h"

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QStandardPaths>
#include <QtTest>

class TestDraftRepository : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
        QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
        // Wipe any prior state from a previous run.
        QFile::remove(fc::cache::Database::filePath());
        fc::cache::Database::initialize();
    }

    void upsertAssignsLocalIdAndPersists() {
        fc::cache::DraftRow d;
        d.subject  = QStringLiteral("hello");
        d.toAddrs  = {QStringLiteral("a@x.test")};
        d.bodyText = QStringLiteral("body");
        d.dirty    = true;

        const QString id = fc::cache::DraftRepository::upsert(d);
        QVERIFY(!id.isEmpty());
        QVERIFY(id.startsWith(QStringLiteral("tmp-")));

        const auto loaded = fc::cache::DraftRepository::byId(id);
        QCOMPARE(loaded.subject,  QStringLiteral("hello"));
        QCOMPARE(loaded.toAddrs.size(), 1);
        QCOMPARE(loaded.toAddrs.front(), QStringLiteral("a@x.test"));
        QCOMPARE(loaded.bodyText, QStringLiteral("body"));
        QVERIFY(loaded.dirty);
    }

    void dirtyDraftsListsOnlyDirty() {
        fc::cache::DraftRow clean;
        clean.subject = QStringLiteral("clean");
        clean.dirty   = false;
        const QString cleanId = fc::cache::DraftRepository::upsert(clean);

        fc::cache::DraftRow dirty;
        dirty.subject = QStringLiteral("dirty");
        dirty.dirty   = true;
        const QString dirtyId = fc::cache::DraftRepository::upsert(dirty);

        const auto dirties = fc::cache::DraftRepository::dirtyDrafts();
        bool foundDirty = false, foundClean = false;
        for (const auto& d : dirties) {
            if (d.id == dirtyId) foundDirty = true;
            if (d.id == cleanId) foundClean = true;
        }
        QVERIFY(foundDirty);
        QVERIFY(!foundClean);
    }

    void markSyncedRekeysToGmailDraftId() {
        fc::cache::DraftRow d;
        d.subject = QStringLiteral("sync me");
        d.dirty   = true;
        const QString localId  = fc::cache::DraftRepository::upsert(d);
        const QString remoteId = QStringLiteral("draft-abc-123");

        fc::cache::DraftRepository::markSynced(localId, remoteId);

        QVERIFY(fc::cache::DraftRepository::byId(localId).id.isEmpty());
        const auto reloaded = fc::cache::DraftRepository::byId(remoteId);
        QCOMPARE(reloaded.id, remoteId);
        QVERIFY(!reloaded.dirty);
    }
};

QTEST_MAIN(TestDraftRepository)
#include "test_draft_repository.moc"
