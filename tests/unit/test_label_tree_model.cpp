#include "cache/Database.h"
#include "cache/LabelRepository.h"
#include "models/LabelTreeModel.h"

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtTest>

namespace fc::cache { QSqlDatabase databaseHandle(); }

class TestLabelTreeModel : public QObject {
    Q_OBJECT
private:
    static constexpr const char* kAccountId =
        "00000000-0000-4000-8000-aaaaaaaaaaa3";

    static void seedAccount() {
        auto db = fc::cache::databaseHandle();
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO accounts(id, email, sort_order, created_at) "
            "VALUES(:id, :em, 0, strftime('%s','now')*1000)"));
        q.bindValue(QStringLiteral(":id"), QString::fromLatin1(kAccountId));
        q.bindValue(QStringLiteral(":em"),
                    QStringLiteral("label-tree@example.test"));
        QVERIFY(q.exec());
    }

    int countDescendants(fc::LabelTreeModel& m, const QModelIndex& parent) {
        int n = m.rowCount(parent);
        int total = n;
        for (int i = 0; i < n; ++i) {
            total += countDescendants(m, m.index(i, 0, parent));
        }
        return total;
    }

    QModelIndex findByName(fc::LabelTreeModel& m, const QString& name,
                           const QModelIndex& parent = {}) {
        const int n = m.rowCount(parent);
        for (int i = 0; i < n; ++i) {
            const auto idx = m.index(i, 0, parent);
            if (idx.data(fc::LabelTreeModel::NameRole).toString() == name) return idx;
            const auto child = findByName(m, name, idx);
            if (child.isValid()) return child;
        }
        return {};
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
        QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
        QFile::remove(fc::cache::Database::filePath());
        fc::cache::Database::initialize();
        seedAccount();

        // Seed: a couple of system labels and a nested user-label hierarchy,
        // all scoped to the test account.
        const QString account = QString::fromLatin1(kAccountId);
        auto sys = [&account](const QString& id, const QString& name) {
            fc::cache::LabelRow r;
            r.accountId = account;
            r.id = id; r.name = name; r.type = QStringLiteral("system");
            return r;
        };
        fc::cache::LabelRepository::upsert(account,
            sys(QStringLiteral("INBOX"),  QStringLiteral("INBOX")));
        fc::cache::LabelRepository::upsert(account,
            sys(QStringLiteral("SENT"),   QStringLiteral("SENT")));
        fc::cache::LabelRepository::upsert(account,
            sys(QStringLiteral("STARRED"),QStringLiteral("STARRED")));

        auto user = [&account](const QString& id, const QString& name) {
            fc::cache::LabelRow r;
            r.accountId = account;
            r.id = id; r.name = name; r.type = QStringLiteral("user");
            return r;
        };
        fc::cache::LabelRepository::upsert(account,
            user(QStringLiteral("Label_1"),
                  QStringLiteral("Travel/Booking")));
        fc::cache::LabelRepository::upsert(account,
            user(QStringLiteral("Label_2"),
                  QStringLiteral("Travel/Receipts")));
        fc::cache::LabelRepository::upsert(account,
            user(QStringLiteral("Label_3"),
                  QStringLiteral("Personal")));
    }

    void buildsSystemLabelsUnderFoldersSectionInOrder() {
        fc::LabelTreeModel m;
        m.setAccountId(QString::fromLatin1(kAccountId));
        // v2 added an "All Inboxes" synthetic node at the top, ahead of
        // the per-account "Folders" / "Labels" sections.
        QCOMPARE(m.rowCount(), 3);
        const auto all = m.index(0, 0);
        QCOMPARE(all.data(fc::LabelTreeModel::TypeRole).toString(),
                 QStringLiteral("synthetic"));
        QCOMPARE(all.data(fc::LabelTreeModel::IdRole).toString(),
                 QStringLiteral("__all_inboxes"));
        const auto folders = m.index(1, 0);
        QCOMPARE(folders.data(fc::LabelTreeModel::TypeRole).toString(),
                 QStringLiteral("section"));
        QCOMPARE(folders.data(fc::LabelTreeModel::NameRole).toString(),
                 QStringLiteral("__folders"));

        QStringList sysIds;
        for (int i = 0; i < m.rowCount(folders); ++i) {
            sysIds << m.index(i, 0, folders)
                       .data(fc::LabelTreeModel::IdRole).toString();
        }
        const int inboxIdx   = sysIds.indexOf(QStringLiteral("INBOX"));
        const int starredIdx = sysIds.indexOf(QStringLiteral("STARRED"));
        const int sentIdx    = sysIds.indexOf(QStringLiteral("SENT"));
        QVERIFY(inboxIdx   >= 0);
        QVERIFY(starredIdx >= 0);
        QVERIFY(sentIdx    >= 0);
        // Canonical order INBOX → STARRED → SENT.
        QVERIFY(inboxIdx < starredIdx);
        QVERIFY(starredIdx < sentIdx);
    }

    void nestsSlashSeparatedUserLabels() {
        fc::LabelTreeModel m;
        m.setAccountId(QString::fromLatin1(kAccountId));
        const auto travel = findByName(m, QStringLiteral("Travel"));
        QVERIFY(travel.isValid());
        QCOMPARE(m.rowCount(travel), 2);

        const auto booking = findByName(m, QStringLiteral("Travel/Booking"));
        QVERIFY(booking.isValid());
        QCOMPARE(booking.data(fc::LabelTreeModel::IdRole).toString(),
                 QStringLiteral("Label_1"));

        // Personal sits one level under the "Labels" section (not at the
        // root, not under Travel).
        const auto personal = findByName(m, QStringLiteral("Personal"));
        QVERIFY(personal.isValid());
        const auto labelsSection = personal.parent();
        QVERIFY(labelsSection.isValid());
        QCOMPARE(labelsSection.data(fc::LabelTreeModel::TypeRole).toString(),
                 QStringLiteral("section"));
        QVERIFY(!labelsSection.parent().isValid());
    }
};

QTEST_MAIN(TestLabelTreeModel)
#include "test_label_tree_model.moc"
