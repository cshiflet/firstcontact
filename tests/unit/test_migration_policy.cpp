#include "cache/Database.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtTest>

#include <cstdio>
#include <cstdlib>

namespace {

constexpr const char* kChildArg = "--stale-schema-child";

void childMessageHandler(QtMsgType type, const QMessageLogContext&,
                         const QString& message) {
    const QByteArray bytes = message.toLocal8Bit();
    std::fprintf(stderr, "%s\n", bytes.constData());
    std::fflush(stderr);
    if (type == QtFatalMsg) std::abort();
}

int runStaleSchemaChild(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
    QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
    qInstallMessageHandler(childMessageHandler);

    const QString path = fc::cache::Database::filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile::remove(path);

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("stale_schema_seed"));
        db.setDatabaseName(path);
        if (!db.open()) return 2;

        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT)"))) {
            return 3;
        }
        if (!q.exec(QStringLiteral(
                "INSERT INTO meta(key, value) VALUES('schema_version', '8')"))) {
            return 4;
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("stale_schema_seed"));

    fc::cache::Database::initialize();
    return 0;
}

}  // namespace

class TestMigrationPolicy : public QObject {
    Q_OBJECT
private slots:
    void oldSchemaVersionIsRejectedWithResetDbMessage() {
        QProcess child;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const QString dataHome = QDir::temp().absoluteFilePath(
            QStringLiteral("firstcontact-stale-schema-%1").arg(
                QCoreApplication::applicationPid()));
        env.insert(QStringLiteral("XDG_DATA_HOME"), dataHome);
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), dataHome + QStringLiteral("/config"));
        child.setProcessEnvironment(env);
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({QString::fromLatin1(kChildArg)});
        child.start();

        QVERIFY2(child.waitForFinished(10000),
                 qPrintable(QStringLiteral("child did not finish: %1")
                     .arg(child.errorString())));
        QVERIFY(child.exitStatus() == QProcess::CrashExit ||
                child.exitCode() != 0);

        const QString output = QString::fromLocal8Bit(child.readAllStandardError())
            + QString::fromLocal8Bit(child.readAllStandardOutput());
        QVERIFY2(output.contains(QStringLiteral("No migration available")),
                 qPrintable(output));
        QVERIFY2(output.contains(QStringLiteral("firstcontact reset-db")),
                 qPrintable(output));
    }
};

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String(kChildArg)) {
            return runStaleSchemaChild(argc, argv);
        }
    }

    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
    QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
    TestMigrationPolicy tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_migration_policy.moc"
