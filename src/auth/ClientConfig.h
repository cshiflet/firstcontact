#pragma once

#include <QString>

namespace fc::auth {

// Holds the user-supplied Google OAuth Desktop-app client_id. Persisted via
// QSettings (NOT the keychain — this is not a secret for installed apps).
class ClientConfig {
public:
    QString clientId() const;
    void setClientId(const QString& id);

    bool isConfigured() const;

    // Convenience: clears the saved client_id (used by Sign Out → Reset).
    void clear();
};

}  // namespace fc::auth
