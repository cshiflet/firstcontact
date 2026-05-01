#pragma once

#include <QString>

namespace fc::auth {

// Holds the user-supplied Google OAuth Desktop-app client_id and client_secret.
//
// Both are persisted via QSettings (which we chmod 0600 in setClientId).
// Google labels the second value a "secret" but their own docs note it
// "isn't a true secret because it's bundled with the source code of the
// application" — Desktop OAuth still requires sending it in the token POST,
// even with PKCE. Storing it in QSettings (single-user-readable) matches the
// trust model: the local user already has access to the binary that would
// embed it anyway.
class ClientConfig {
public:
    QString clientId() const;
    void setClientId(const QString& id);

    QString clientSecret() const;
    void setClientSecret(const QString& secret);

    // True only when BOTH the id and the secret have been recorded — Google's
    // token endpoint rejects requests without the secret with
    // "invalid_request: client_secret is missing.".
    bool isConfigured() const;

    void clear();
};

}  // namespace fc::auth
