#include "ClientConfig.h"

#include "util/Paths.h"

#include <QSettings>

namespace fc::auth {

namespace {
constexpr char kIdKey[]     = "oauth/client_id";
constexpr char kSecretKey[] = "oauth/client_secret";
}  // namespace

QString ClientConfig::clientId() const {
    QSettings s;
    return s.value(QLatin1String(kIdKey)).toString();
}

void ClientConfig::setClientId(const QString& id) {
    QSettings s;
    s.setValue(QLatin1String(kIdKey), id);
    s.sync();
    fc::util::restrictPermissionsToOwner(s.fileName());
}

QString ClientConfig::clientSecret() const {
    QSettings s;
    return s.value(QLatin1String(kSecretKey)).toString();
}

void ClientConfig::setClientSecret(const QString& secret) {
    QSettings s;
    s.setValue(QLatin1String(kSecretKey), secret);
    s.sync();
    fc::util::restrictPermissionsToOwner(s.fileName());
}

bool ClientConfig::isConfigured() const {
    return !clientId().isEmpty() && !clientSecret().isEmpty();
}

void ClientConfig::clear() {
    QSettings s;
    s.remove(QLatin1String(kIdKey));
    s.remove(QLatin1String(kSecretKey));
}

}  // namespace fc::auth
