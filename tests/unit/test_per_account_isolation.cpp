// Per-account isolation: rows inserted under account A must never appear
// in queries scoped to account B (and vice versa). This guards against the
// "we forgot a WHERE account_id = …" class of bug after the v6 migration.

#include "cache/Database.h"
#include "cache/DraftRepository.h"
#include "cache/LabelRepository.h"
#include "cache/MessageRepository.h"
#include "cache/MetaRepository.h"
#include "cache/OutboxRepository.h"
#include "cache/PendingOpsRepository.h"
#include "models/Message.h"

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtTest>

namespace {

const QString kAccountA = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QString kAccountB = QStringLiteral("22222222-2222-4222-8222-222222222222");

void seedAccount(const QString& accountId, const QString& email) {
    auto db = QSqlDatabase::database(fc::cache::Database::connectionName());
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO accounts(id, email, sort_order, created_at) "
        "VALUES(:id, :em, 0, strftime('%s','now')*1000)"));
    q.bindValue(QStringLiteral(":id"), accountId);
    q.bindValue(QStringLiteral(":em"), email);
    QVERIFY(q.exec());
}

fc::Message makeMessage(const QString& accountId, const QString& msgId,
                         const QString& threadId, const QString& subject) {
    fc::Message m;
    m.accountId    = accountId;
    m.id           = msgId;
    m.threadId     = threadId;
    m.internalDate = 1700000000000LL;
    m.subject      = subject;
    m.snippet      = subject;
    m.fromAddr     = QStringLiteral("a@b.test");
    m.fromName     = QStringLiteral("Alice");
    m.bodyText     = QStringLiteral("body of ") + subject;
    return m;
}

fc::cache::LabelRow makeLabel(const QString& accountId, const QString& id,
                               const QString& name) {
    fc::cache::LabelRow l;
    l.accountId = accountId;
    l.id        = id;
    l.name      = name;
    l.type      = QStringLiteral("system");
    return l;
}

}  // namespace

class TestPerAccountIsolation : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
        QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
        QFile::remove(fc::cache::Database::filePath());
        fc::cache::Database::initialize();

        // The migration seeds a "legacy" account; we add two more so each
        // test slot has its own pair.
        seedAccount(kAccountA, QStringLiteral("a@accounta.test"));
        seedAccount(kAccountB, QStringLiteral("b@accountb.test"));
    }

    void labelsAreNotSharedAcrossAccounts() {
        // INBOX in account A is colored red; INBOX in account B is colored
        // blue. The same id "INBOX" exists under both — composite PK keeps
        // them distinct.
        auto la = makeLabel(kAccountA, QStringLiteral("INBOX"),
                            QStringLiteral("Inbox"));
        la.colorBg = QStringLiteral("#ff0000");
        fc::cache::LabelRepository::upsert(kAccountA, la);

        auto lb = makeLabel(kAccountB, QStringLiteral("INBOX"),
                            QStringLiteral("Inbox"));
        lb.colorBg = QStringLiteral("#0000ff");
        fc::cache::LabelRepository::upsert(kAccountB, lb);

        const auto a = fc::cache::LabelRepository::byId(kAccountA, QStringLiteral("INBOX"));
        const auto b = fc::cache::LabelRepository::byId(kAccountB, QStringLiteral("INBOX"));
        QCOMPARE(a.colorBg, QStringLiteral("#ff0000"));
        QCOMPARE(b.colorBg, QStringLiteral("#0000ff"));

        // all() is per-account.
        const auto allA = fc::cache::LabelRepository::all(kAccountA);
        const auto allB = fc::cache::LabelRepository::all(kAccountB);
        QVERIFY(!allA.empty());
        QVERIFY(!allB.empty());
        for (const auto& r : allA) QCOMPARE(r.accountId, kAccountA);
        for (const auto& r : allB) QCOMPARE(r.accountId, kAccountB);
    }

    void messagesAreNotSharedAcrossAccounts() {
        // Pre-req: each account needs INBOX in labels (FK from
        // message_labels). seedLabels above did that for account A and
        // account B, so we can attach the labels we want.
        auto inboxA = makeLabel(kAccountA, QStringLiteral("INBOX"),
                                 QStringLiteral("Inbox"));
        fc::cache::LabelRepository::upsert(kAccountA, inboxA);
        auto inboxB = makeLabel(kAccountB, QStringLiteral("INBOX"),
                                 QStringLiteral("Inbox"));
        fc::cache::LabelRepository::upsert(kAccountB, inboxB);

        auto mA = makeMessage(kAccountA, QStringLiteral("msg-A"),
                              QStringLiteral("thr-A"), QStringLiteral("Hello A"));
        mA.labelIds = {QStringLiteral("INBOX")};
        QVERIFY(fc::cache::MessageRepository::upsert(kAccountA, mA) > 0);

        auto mB = makeMessage(kAccountB, QStringLiteral("msg-B"),
                              QStringLiteral("thr-B"), QStringLiteral("Hello B"));
        mB.labelIds = {QStringLiteral("INBOX")};
        QVERIFY(fc::cache::MessageRepository::upsert(kAccountB, mB) > 0);

        // listByLabel for INBOX scoped to A returns only msg-A.
        const auto listA = fc::cache::MessageRepository::listByLabel(
            kAccountA, QStringLiteral("INBOX"), 100, 0);
        QCOMPARE(int(listA.size()), 1);
        QCOMPARE(listA.front().id, QStringLiteral("msg-A"));
        QCOMPARE(listA.front().accountId, kAccountA);

        const auto listB = fc::cache::MessageRepository::listByLabel(
            kAccountB, QStringLiteral("INBOX"), 100, 0);
        QCOMPARE(int(listB.size()), 1);
        QCOMPARE(listB.front().id, QStringLiteral("msg-B"));
        QCOMPARE(listB.front().accountId, kAccountB);

        // byId is per-account too. Looking up msg-A under account B
        // returns an empty message (the row exists but the WHERE filter
        // hides it).
        const auto missing = fc::cache::MessageRepository::byId(
            kAccountB, QStringLiteral("msg-A"));
        QVERIFY(missing.id.isEmpty());

        // FTS5 search: the new account_id UNINDEXED column scopes results.
        const auto ftsA = fc::cache::MessageRepository::searchFts(
            kAccountA, QStringLiteral("Hello"), 100);
        const auto ftsB = fc::cache::MessageRepository::searchFts(
            kAccountB, QStringLiteral("Hello"), 100);
        QCOMPARE(int(ftsA.size()), 1);
        QCOMPARE(ftsA.front().id, QStringLiteral("msg-A"));
        QCOMPARE(int(ftsB.size()), 1);
        QCOMPARE(ftsB.front().id, QStringLiteral("msg-B"));
    }

    void accountMetaIsScopedPerAccount() {
        fc::cache::MetaRepository::set(kAccountA,
                                       QStringLiteral("history_id"),
                                       QStringLiteral("1001"));
        fc::cache::MetaRepository::set(kAccountB,
                                       QStringLiteral("history_id"),
                                       QStringLiteral("2002"));

        QCOMPARE(fc::cache::MetaRepository::historyId(kAccountA),
                 QStringLiteral("1001"));
        QCOMPARE(fc::cache::MetaRepository::historyId(kAccountB),
                 QStringLiteral("2002"));

        // Setting on A doesn't bleed into B.
        fc::cache::MetaRepository::set(kAccountA,
                                       QStringLiteral("history_id"),
                                       QStringLiteral("1100"));
        QCOMPARE(fc::cache::MetaRepository::historyId(kAccountA),
                 QStringLiteral("1100"));
        QCOMPARE(fc::cache::MetaRepository::historyId(kAccountB),
                 QStringLiteral("2002"));
    }

    void outboxAndPendingOpsAreScopedPerAccount() {
        fc::cache::OutboxItem itA;
        itA.rfc5322 = QByteArray("from A");
        const qint64 idA = fc::cache::OutboxRepository::enqueue(kAccountA, itA);
        QVERIFY(idA > 0);

        fc::cache::OutboxItem itB;
        itB.rfc5322 = QByteArray("from B");
        const qint64 idB = fc::cache::OutboxRepository::enqueue(kAccountB, itB);
        QVERIFY(idB > 0);

        const auto dueA = fc::cache::OutboxRepository::dueForSend(kAccountA);
        const auto dueB = fc::cache::OutboxRepository::dueForSend(kAccountB);
        bool seenAonA = false, seenBonA = false, seenAonB = false, seenBonB = false;
        for (const auto& it : dueA) {
            if (it.id == idA) seenAonA = true;
            if (it.id == idB) seenBonA = true;
        }
        for (const auto& it : dueB) {
            if (it.id == idA) seenAonB = true;
            if (it.id == idB) seenBonB = true;
        }
        QVERIFY(seenAonA);
        QVERIFY(!seenBonA);
        QVERIFY(!seenAonB);
        QVERIFY(seenBonB);

        // The cross-account variant exposes both, with each row carrying
        // the right accountId.
        const auto allDue = fc::cache::OutboxRepository::dueForSendAllAccounts();
        bool foundA = false, foundB = false;
        for (const auto& it : allDue) {
            if (it.id == idA) { foundA = true; QCOMPARE(it.accountId, kAccountA); }
            if (it.id == idB) { foundB = true; QCOMPARE(it.accountId, kAccountB); }
        }
        QVERIFY(foundA);
        QVERIFY(foundB);
    }

    void draftsAreScopedPerAccount() {
        fc::cache::DraftRow dA;
        dA.subject = QStringLiteral("draft A");
        dA.dirty   = true;
        const QString draftIdA = fc::cache::DraftRepository::upsert(kAccountA, dA);
        QVERIFY(!draftIdA.isEmpty());

        fc::cache::DraftRow dB;
        dB.subject = QStringLiteral("draft B");
        dB.dirty   = true;
        const QString draftIdB = fc::cache::DraftRepository::upsert(kAccountB, dB);
        QVERIFY(!draftIdB.isEmpty());

        const auto listA = fc::cache::DraftRepository::dirtyDrafts(kAccountA);
        bool foundA = false, foundB = false;
        for (const auto& d : listA) {
            if (d.id == draftIdA) foundA = true;
            if (d.id == draftIdB) foundB = true;
        }
        QVERIFY(foundA);
        QVERIFY(!foundB);

        const auto all = fc::cache::DraftRepository::dirtyDraftsAllAccounts();
        bool seenA = false, seenB = false;
        for (const auto& d : all) {
            if (d.accountId == kAccountA) seenA = true;
            if (d.accountId == kAccountB) seenB = true;
        }
        QVERIFY(seenA);
        QVERIFY(seenB);
    }
};

QTEST_MAIN(TestPerAccountIsolation)
#include "test_per_account_isolation.moc"
