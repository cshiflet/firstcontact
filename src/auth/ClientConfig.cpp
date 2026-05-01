#include "ClientConfig.h"

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
}

bool ClientConfig::isConfigured() const {
    return !clientId().isEmpty();
}

void ClientConfig::clear() {
    QSettings s;
    s.remove(QLatin1String(kKey));
}

}  // namespace fc::auth
