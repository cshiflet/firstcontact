#include "Paths.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
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

void restrictPermissionsToOwner(const QString& path) {
    if (!QFileInfo::exists(path)) return;
    QFile f(path);
    const QFileDevice::Permissions wanted =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!f.setPermissions(wanted)) {
        qWarning("Failed to set 0600 permissions on %s", qUtf8Printable(path));
    }
}

}  // namespace fc::util
