#include "cache/Database.h"
#include "cache/LabelRepository.h"
#include "models/LabelTreeModel.h"

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QStandardPaths>
#include <QtTest>

class TestLabelTreeModel : public QObject {
    Q_OBJECT
private:
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

        // Seed: a couple of system labels and a nested user-label hierarchy.
        auto sys = [](const QString& id, const QString& name) {
            fc::cache::LabelRow r;
            r.id = id; r.name = name; r.type = QStringLiteral("system");
            return r;
        };
        fc::cache::LabelRepository::upsert(sys(QStringLiteral("INBOX"),  QStringLiteral("INBOX")));
        fc::cache::LabelRepository::upsert(sys(QStringLiteral("SENT"),   QStringLiteral("SENT")));
        fc::cache::LabelRepository::upsert(sys(QStringLiteral("STARRED"),QStringLiteral("STARRED")));

        auto user = [](const QString& id, const QString& name) {
            fc::cache::LabelRow r;
            r.id = id; r.name = name; r.type = QStringLiteral("user");
            return r;
        };
        fc::cache::LabelRepository::upsert(user(QStringLiteral("Label_1"),
                                                QStringLiteral("Travel/Booking")));
        fc::cache::LabelRepository::upsert(user(QStringLiteral("Label_2"),
                                                QStringLiteral("Travel/Receipts")));
        fc::cache::LabelRepository::upsert(user(QStringLiteral("Label_3"),
                                                QStringLiteral("Personal")));
    }

    void buildsSystemLabelsUnderFoldersSectionInOrder() {
        fc::LabelTreeModel m;
        // Top level is now exactly the two synthetic section parents,
        // "Folders" and "Labels", each with TypeRole == "section".
        QCOMPARE(m.rowCount(), 2);
        const auto folders = m.index(0, 0);
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
