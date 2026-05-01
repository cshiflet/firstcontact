#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace fc::auth {

// Cross-platform secure token storage backed by QtKeychain.
//
// All operations are async (QtKeychain uses platform IPC under the hood).
// Callbacks fire on the calling thread's event loop.
class TokenStore : public QObject {
    Q_OBJECT
public:
    explicit TokenStore(QObject* parent = nullptr);

    // Per-account record stored in the keychain.
    struct Tokens {
        QString accessToken;
        QString refreshToken;
        qint64  expiresAtUnix = 0;   // seconds since epoch
        QString accountEmail;
        bool valid() const { return !refreshToken.isEmpty(); }
    };

    using Callback = std::function<void(bool ok, Tokens, QString error)>;

    void load(Callback cb);
    void save(const Tokens& t, std::function<void(bool ok, QString error)> cb);
    void erase(std::function<void(bool ok, QString error)> cb);

private:
    static QString serviceName();
    static QString keyName();
};

}  // namespace fc::auth
