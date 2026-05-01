#include "Paths.h"

#include <QDir>
#include <QStandardPaths>

namespace fc::util {

namespace {
QString ensure(const QString& path) {
    QDir().mkpath(path);
    return path;
}
}  // namespace

QString dataDir() {
    return ensure(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
}

QString configDir() {
    return ensure(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
}

QString logDir() {
    return ensure(dataDir() + QStringLiteral("/logs"));
}

QString cacheDbPath() {
    return dataDir() + QStringLiteral("/cache.db");
}

QString singleInstanceLockPath() {
    return dataDir() + QStringLiteral("/firstcontact.lock");
}

}  // namespace fc::util
