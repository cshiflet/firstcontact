#include "ClientConfig.h"

#include "util/Paths.h"

#include <QSettings>

namespace fc::auth {

namespace {
constexpr char kKey[] = "oauth/client_id";
}

QString ClientConfig::clientId() const {
    QSettings s;
    return s.value(QLatin1String(kKey)).toString();
}

void ClientConfig::setClientId(const QString& id) {
    QSettings s;
    s.setValue(QLatin1String(kKey), id);
    s.sync();
    // client_id is not a secret per OAuth Desktop-app spec, but defense in
    // depth — keep this file out of other local users' reach.
    fc::util::restrictPermissionsToOwner(s.fileName());
}

bool ClientConfig::isConfigured() const {
    return !clientId().isEmpty();
}

void ClientConfig::clear() {
    QSettings s;
    s.remove(QLatin1String(kKey));
}

}  // namespace fc::auth
