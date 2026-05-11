#include "cache/Database.h"
#include "cache/PendingOpsRepository.h"

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtTest>

namespace fc::cache { QSqlDatabase databaseHandle(); }

class TestPendingOpsRepository : public QObject {
    Q_OBJECT
private:
    static constexpr const char* kAccountId =
        "00000000-0000-4000-8000-aaaaaaaaaaa2";

    static void seedAccount() {
        auto db = fc::cache::databaseHandle();
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO accounts(id, email, sort_order, created_at) "
            "VALUES(:id, :em, 0, strftime('%s','now')*1000)"));
        q.bindValue(QStringLiteral(":id"), QString::fromLatin1(kAccountId));
        q.bindValue(QStringLiteral(":em"),
                    QStringLiteral("pending-test@example.test"));
        QVERIFY(q.exec());
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
        QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
        QFile::remove(fc::cache::Database::filePath());
        fc::cache::Database::initialize();
        seedAccount();
    }

    void enqueuesAndDecodesLabelLists() {
        const QString account = QString::fromLatin1(kAccountId);
        const qint64 id = fc::cache::PendingOpsRepository::enqueueModify(
            account,
            QStringLiteral("msg-1"),
            {QStringLiteral("STARRED"), QStringLiteral("Label_42")},
            {QStringLiteral("UNREAD")});
        QVERIFY(id > 0);

        const auto due = fc::cache::PendingOpsRepository::due(account);
        bool found = false;
        for (const auto& op : due) {
            if (op.id != id) continue;
            found = true;
            QCOMPARE(op.opType,    QStringLiteral("modify"));
            QCOMPARE(op.messageId, QStringLiteral("msg-1"));
            QCOMPARE(op.addLabels.size(),    2);
            QCOMPARE(op.removeLabels.size(), 1);
            QVERIFY(op.addLabels.contains(QStringLiteral("STARRED")));
            QVERIFY(op.removeLabels.contains(QStringLiteral("UNREAD")));
        }
        QVERIFY(found);
    }

    void markAttemptIncrementsCounter() {
        const QString account = QString::fromLatin1(kAccountId);
        const qint64 id = fc::cache::PendingOpsRepository::enqueueModify(
            account, QStringLiteral("msg-2"), {QStringLiteral("STARRED")}, {});
        fc::cache::PendingOpsRepository::markAttempt(id, QStringLiteral("transient"));
        fc::cache::PendingOpsRepository::markAttempt(id, QStringLiteral("transient"));

        for (const auto& op : fc::cache::PendingOpsRepository::due(account)) {
            if (op.id == id) {
                QCOMPARE(op.attempts,   2);
                QCOMPARE(op.lastError,  QStringLiteral("transient"));
                return;
            }
        }
        QFAIL("op was not in due list");
    }

    void removeDeletesRow() {
        const QString account = QString::fromLatin1(kAccountId);
        const qint64 id = fc::cache::PendingOpsRepository::enqueueModify(
            account, QStringLiteral("msg-3"), {}, {QStringLiteral("INBOX")});
        fc::cache::PendingOpsRepository::remove(id);
        for (const auto& op : fc::cache::PendingOpsRepository::due(account)) {
            QVERIFY(op.id != id);
        }
    }
};

QTEST_MAIN(TestPendingOpsRepository)
#include "test_pending_ops_repository.moc"
