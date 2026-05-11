#include "OAuthClient.h"

#include "ClientConfig.h"
#include "util/Base64Url.h"
#include "util/Browser.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOAuth2AuthorizationCodeFlow>
#include <QOAuthHttpServerReplyHandler>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>

namespace fc::auth {

namespace {

constexpr char kAuthEndpoint[]  = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr char kTokenEndpoint[] = "https://oauth2.googleapis.com/token";
constexpr char kRevokeEndpoint[]= "https://oauth2.googleapis.com/revoke";

const QString& kScopeString() {
    static const QString s = QStringLiteral(
        "https://www.googleapis.com/auth/gmail.readonly "
        "https://www.googleapis.com/auth/gmail.send "
        "https://www.googleapis.com/auth/gmail.modify "
        "https://www.googleapis.com/auth/gmail.compose "
        "https://www.googleapis.com/auth/gmail.labels "
        "https://www.googleapis.com/auth/userinfo.email");
    return s;
}

QByteArray makeCodeVerifier() {
    // 64 bytes → 86 base64url chars; well within RFC 7636's 43–128 window.
    QByteArray raw(64, Qt::Uninitialized);
    auto* gen = QRandomGenerator::system();
    for (int i = 0; i < raw.size(); i += 4) {
        const quint32 r = gen->generate();
        for (int j = 0; j < 4 && i + j < raw.size(); ++j) {
            raw[i + j] = static_cast<char>((r >> (j * 8)) & 0xff);
        }
    }
    return util::base64UrlEncode(raw);
}

QByteArray makeCodeChallenge(const QByteArray& verifier) {
    return util::base64UrlEncode(QCryptographicHash::hash(verifier, QCryptographicHash::Sha256));
}

}  // namespace

struct OAuthClient::Impl {
    ClientConfig* config = nullptr;
    TokenStore*   store  = nullptr;
    QString       accountId;   // empty for unbound (pre-sign-in) clients

    QOAuthHttpServerReplyHandler*   handler = nullptr;
    QNetworkAccessManager*          nam     = nullptr;

    // Per-flow state — regenerated on every call to authorize(). The state
    // string is what Google echoes back in the redirect; we verify it matches
    // to defeat CSRF. callbackConsumed flips to true once we've successfully
    // pulled the auth code out of a callback so subsequent stray requests to
    // the loopback port (browsers love to follow up with /favicon.ico after
    // a redirect) don't trigger a second state-mismatch failure.
    QByteArray codeVerifier;
    QString    state;
    bool       callbackConsumed = false;

    mutable QMutex tokenMutex;
    TokenStore::Tokens tokens;
};

OAuthClient::OAuthClient(ClientConfig* config, TokenStore* store,
                          QString accountId, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->config    = config;
    d_->store     = store;
    d_->accountId = std::move(accountId);
    d_->nam       = new QNetworkAccessManager(this);

    hydrateFromStore();
}

OAuthClient::~OAuthClient() { delete d_; }

QString OAuthClient::accountId() const {
    QMutexLocker lock(&d_->tokenMutex);
    return d_->accountId;
}

TokenStore::Tokens OAuthClient::tokensSnapshot() const {
    QMutexLocker lock(&d_->tokenMutex);
    return d_->tokens;
}

void OAuthClient::adoptTokens(const TokenStore::Tokens& tokens) {
    QString aid;
    {
        QMutexLocker lock(&d_->tokenMutex);
        if (d_->accountId.isEmpty()) {
            qWarning("OAuthClient::adoptTokens called on an unbound client; "
                     "use bindAccountId instead");
            return;
        }
        d_->tokens             = tokens;
        d_->tokens.accountId   = d_->accountId;   // overwrite — bound takes precedence
        aid                    = d_->accountId;
    }
    if (!d_->tokens.valid()) return;
    auto persisted = d_->tokens;
    d_->store->save(persisted, [aid](bool ok, QString err) {
        if (!ok) qWarning("TokenStore::save (adoptTokens for %s) failed: %s",
                          qUtf8Printable(aid),
                          qUtf8Printable(err));
    });
    emit tokensLoaded();
}

void OAuthClient::bindAccountId(const QString& accountId) {
    if (accountId.isEmpty()) {
        qWarning("OAuthClient::bindAccountId: refusing to bind to empty id");
        return;
    }
    TokenStore::Tokens snapshot;
    {
        QMutexLocker lock(&d_->tokenMutex);
        if (!d_->accountId.isEmpty() && d_->accountId != accountId) {
            qWarning("OAuthClient::bindAccountId: already bound to %s; "
                     "refusing rebind to %s",
                     qUtf8Printable(d_->accountId),
                     qUtf8Printable(accountId));
            return;
        }
        d_->accountId         = accountId;
        d_->tokens.accountId  = accountId;
        snapshot              = d_->tokens;
    }
    if (!snapshot.valid()) return;   // nothing to persist yet
    d_->store->save(snapshot, [](bool ok, QString err) {
        if (!ok) qWarning("TokenStore::save (bindAccountId) failed: %s",
                          qUtf8Printable(err));
    });
}

void OAuthClient::hydrateFromStore() {
    if (d_->accountId.isEmpty()) {
        // Unbound (sign-in-not-yet-complete) client — nothing to load.
        emit tokensLoaded();
        return;
    }
    d_->store->load(d_->accountId,
        [this](bool ok, TokenStore::Tokens t, QString err) {
        if (!ok) {
            qWarning("TokenStore::load failed: %s", qUtf8Printable(err));
            emit tokensLoaded();   // tell the UI to stop assuming pending
            return;
        }
        {
            QMutexLocker lock(&d_->tokenMutex);
            // Don't clobber valid in-memory tokens with an empty-slot
            // result. The Add-account flow constructs a bound
            // AccountContext (which schedules this load) and then
            // adoptTokens()es the freshly-issued tokens onto it
            // synchronously. If the load callback fires AFTER
            // adoptTokens, an empty slot result would overwrite the
            // valid tokens we just put in place.
            if (t.valid() || !d_->tokens.valid()) {
                d_->tokens = std::move(t);
            }
        }
        emit tokensLoaded();
    });
}

bool OAuthClient::isAuthorized() const {
    QMutexLocker lock(&d_->tokenMutex);
    return d_->tokens.valid();
}

QString OAuthClient::accountEmail() const {
    QMutexLocker lock(&d_->tokenMutex);
    return d_->tokens.accountEmail;
}

void OAuthClient::setAccountEmail(const QString& email) {
    TokenStore::Tokens snapshot;
    {
        QMutexLocker lock(&d_->tokenMutex);
        if (d_->tokens.accountEmail == email) return;   // no-op, no save
        d_->tokens.accountEmail = email;
        snapshot = d_->tokens;
    }
    // Unbound clients hold the email in memory until bindAccountId
    // runs; persistence has to wait until we know which slot to write
    // it to.
    if (snapshot.accountId.isEmpty()) return;
    d_->store->save(snapshot, [](bool ok, QString err) {
        if (!ok) qWarning("TokenStore::save (email) failed: %s",
                          qUtf8Printable(err));
    });
}

void OAuthClient::ensureHandler() {
    // Recreate the loopback handler on every authorize() call so a previous
    // flow that was abandoned (browser closed, network hiccup) doesn't leave
    // a stale listener around.
    if (d_->handler) {
        d_->handler->close();
        d_->handler->deleteLater();
        d_->handler = nullptr;
    }
    d_->handler = new QOAuthHttpServerReplyHandler(0, this);
    if (!d_->handler->isListening()) {
        qWarning("OAuth loopback handler failed to bind a port — "
                 "the redirect after Google consent will time out");
        return;
    }
    qInfo("OAuth loopback handler listening on %s",
          qUtf8Printable(d_->handler->callback()));

    // Connect to the raw callback signal so we can run our own token
    // exchange. We deliberately do NOT use QOAuth2AuthorizationCodeFlow for
    // the code→token POST: Qt 6.4's flow has a habit of attaching a Basic
    // auth header with empty client_secret, which Google rejects for
    // Desktop OAuth clients with PKCE ("invalid_client") and surfaces only
    // as the cryptic "Unexpected call" / "server replied:" warnings.
    connect(d_->handler, &QOAuthHttpServerReplyHandler::callbackReceived,
            this, &OAuthClient::onAuthCodeCallback);
}

void OAuthClient::authorize() {
    if (!d_->config->isConfigured()) {
        emit failed(tr("OAuth client_id is not configured. Run the setup wizard."));
        return;
    }

    d_->codeVerifier     = makeCodeVerifier();
    d_->callbackConsumed = false;
    {
        QByteArray raw(16, Qt::Uninitialized);
        auto* gen = QRandomGenerator::system();
        for (int i = 0; i < raw.size(); i += 4) {
            const quint32 r = gen->generate();
            for (int j = 0; j < 4 && i + j < raw.size(); ++j) {
                raw[i + j] = static_cast<char>((r >> (j * 8)) & 0xff);
            }
        }
        d_->state = QString::fromLatin1(util::base64UrlEncode(raw));
    }

    const QByteArray challenge = makeCodeChallenge(d_->codeVerifier);

    ensureHandler();
    if (!d_->handler->isListening()) {
        emit failed(tr("Could not bind a local port for the OAuth callback."));
        return;
    }
    const QString redirectUri = d_->handler->callback();

    QUrl authUrl{QLatin1String(kAuthEndpoint)};
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("client_id"),             d_->config->clientId());
    q.addQueryItem(QStringLiteral("redirect_uri"),          redirectUri);
    q.addQueryItem(QStringLiteral("response_type"),         QStringLiteral("code"));
    q.addQueryItem(QStringLiteral("scope"),                 kScopeString());
    q.addQueryItem(QStringLiteral("state"),                 d_->state);
    q.addQueryItem(QStringLiteral("code_challenge"),        QString::fromLatin1(challenge));
    q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    q.addQueryItem(QStringLiteral("access_type"),           QStringLiteral("offline"));
    q.addQueryItem(QStringLiteral("prompt"),                QStringLiteral("consent"));
    authUrl.setQuery(q);

    // Log only the path, not the query — `state` and `code_challenge`
    // are present in the query and should not land in plaintext logs
    // that might be shared with support / pasted into bug reports.
    {
        QUrl scrubbed = authUrl;
        scrubbed.setQuery(QString());
        qInfo("OAuth authorize URL: %s (query redacted)",
              qUtf8Printable(scrubbed.toString()));
    }
    // Hand the URL to the UI and stop there — we deliberately do NOT
    // auto-launch a browser. The dialog the UI puts up offers an
    // explicit "Open in Browser" button plus a copy field, so the user
    // is in control of which browser receives the URL (and so we
    // don't surprise them with a popup window during what looked like
    // a benign click). The `openedAutomatically=false` signal value
    // tells the dialog to phrase its prompt accordingly.
    emit browserAuthRequested(authUrl, /*openedAutomatically=*/false);
}

void OAuthClient::onAuthCodeCallback(const QVariantMap& params) {
    qInfo("OAuth callback received with keys: [%s]",
          qUtf8Printable(params.keys().join(QChar(','))));

    // Browsers commonly hit the loopback port a second time (favicon, prefetch,
    // beacon) after the real OAuth redirect has been served. Those follow-up
    // requests carry no `state` and no `code`, so they'd otherwise spuriously
    // trip the state-mismatch path and emit a confusing `failed` after a
    // successful sign-in. Drop them.
    if (d_->callbackConsumed) {
        qInfo("OAuth: ignoring stray loopback request after successful callback");
        return;
    }

    if (params.contains(QStringLiteral("error"))) {
        const QString err  = params.value(QStringLiteral("error")).toString();
        const QString desc = params.value(QStringLiteral("error_description")).toString();
        qWarning("OAuth callback error: %s — %s",
                 qUtf8Printable(err), qUtf8Printable(desc));
        d_->callbackConsumed = true;
        emit failed(err + QStringLiteral(": ") + desc);
        return;
    }

    const QString receivedState = params.value(QStringLiteral("state")).toString();
    const QString code          = params.value(QStringLiteral("code")).toString();

    // A valid OAuth redirect always carries both state and code. If either is
    // missing we're either looking at a stray browser request or a malformed
    // redirect — either way, don't blow the flow up with a state-mismatch
    // dialog; just log and keep waiting for the real one.
    if (receivedState.isEmpty() && code.isEmpty()) {
        qInfo("OAuth: empty loopback request — likely a browser favicon or "
              "prefetch follow-up; continuing to wait for the real callback");
        return;
    }

    if (receivedState != d_->state) {
        // Log the FACT of the mismatch but never the state values
        // themselves — those are short-lived secrets and pasting
        // them into a support thread would be a real footgun.
        qWarning("OAuth state mismatch — sign-in aborted (state lengths %lld vs %lld)",
                 static_cast<long long>(receivedState.size()),
                 static_cast<long long>(d_->state.size()));
        d_->callbackConsumed = true;
        emit failed(tr("OAuth state mismatch — sign-in aborted for safety."));
        return;
    }

    if (code.isEmpty()) {
        d_->callbackConsumed = true;
        emit failed(tr("OAuth callback missing 'code' parameter."));
        return;
    }

    d_->callbackConsumed = true;
    exchangeCodeForTokens(code);
}

void OAuthClient::exchangeCodeForTokens(const QString& code) {
    const QString clientSecret = d_->config->clientSecret();
    qInfo("Token exchange: client_id=%s…, client_secret=%s",
          d_->config->clientId().left(16).toUtf8().constData(),
          clientSecret.isEmpty() ? "(MISSING — check Settings)" : "(present)");

    QUrlQuery body;
    body.addQueryItem(QStringLiteral("client_id"),     d_->config->clientId());
    body.addQueryItem(QStringLiteral("client_secret"), clientSecret);
    body.addQueryItem(QStringLiteral("code"),          code);
    body.addQueryItem(QStringLiteral("code_verifier"),
                      QString::fromLatin1(d_->codeVerifier));
    body.addQueryItem(QStringLiteral("redirect_uri"),  d_->handler->callback());
    body.addQueryItem(QStringLiteral("grant_type"),    QStringLiteral("authorization_code"));

    QNetworkRequest req{QUrl(QLatin1String(kTokenEndpoint))};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    req.setRawHeader("Accept", "application/json");

    auto* reply = d_->nam->post(req, body.toString(QUrl::FullyEncoded).toUtf8());
    // OAuthClient is Bootstrap-owned for the app's lifetime, so the receiver
    // we attach to (`this`) cannot outlive the connect — capturing `this`
    // directly is safe here.
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray data   = reply->readAll();
        const int status        = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError nerr = reply->error();
        const QString errString = reply->errorString();
        reply->deleteLater();

        qInfo("Token exchange reply: HTTP %d, error %d (%s)",
              status, int(nerr), qUtf8Printable(errString));

        if (nerr != QNetworkReply::NoError) {
            // Log the high-level shape but NEVER the raw response
            // body — Google's token-exchange responses occasionally
            // echo back fields containing the client_secret in
            // verbatim error_description text, and we don't want
            // those landing in user-shareable logs. JSON-shape
            // metadata is safe to log.
            const auto errObj = QJsonDocument::fromJson(data).object();
            qWarning("Token exchange failed: HTTP %d, error %d (%s), error=%s",
                     status, int(nerr), qUtf8Printable(errString),
                     qUtf8Printable(errObj.value(QStringLiteral("error")).toString()));
            // Even on HTTP errors Google typically returns a JSON error body —
            // surface it to the user verbatim so they can act on it.
            QString message = errString;
            const auto err = QJsonDocument::fromJson(data).object();
            if (err.contains(QStringLiteral("error"))) {
                message = err.value(QStringLiteral("error")).toString();
                if (err.contains(QStringLiteral("error_description"))) {
                    message += QStringLiteral(": ") +
                               err.value(QStringLiteral("error_description")).toString();
                }
            }
            emit failed(QStringLiteral("Token exchange failed: %1").arg(message));
            return;
        }

        const auto o = QJsonDocument::fromJson(data).object();
        if (o.contains(QStringLiteral("error"))) {
            emit failed(o.value(QStringLiteral("error")).toString() +
                        QStringLiteral(": ") +
                        o.value(QStringLiteral("error_description")).toString());
            return;
        }

        TokenStore::Tokens t;
        t.accessToken   = o.value(QStringLiteral("access_token")).toString();
        t.refreshToken  = o.value(QStringLiteral("refresh_token")).toString();
        t.expiresAtUnix = QDateTime::currentSecsSinceEpoch()
                        + static_cast<qint64>(o.value(QStringLiteral("expires_in")).toDouble());
        if (t.accessToken.isEmpty() || t.refreshToken.isEmpty()) {
            emit failed(QStringLiteral(
                "Token exchange succeeded but response was missing tokens."));
            return;
        }

        {
            QMutexLocker lock(&d_->tokenMutex);
            t.accountEmail = d_->tokens.accountEmail;  // preserve if any
            // For bound clients, stamp the tokens with our slot id so
            // TokenStore::save targets the right slot; for unbound
            // clients this stays empty until bindAccountId runs.
            t.accountId    = d_->accountId;
            d_->tokens = t;
        }
        // Unbound clients can't persist yet — bindAccountId will save
        // once the caller has minted the accounts row.
        if (!t.accountId.isEmpty()) {
            d_->store->save(t, [](bool ok, QString err) {
                if (!ok) qWarning("TokenStore::save failed: %s",
                                  qUtf8Printable(err));
            });
        }

        // The handler isn't needed once tokens are in hand; closing it
        // releases the loopback port.
        if (d_->handler) {
            d_->handler->close();
            d_->handler->deleteLater();
            d_->handler = nullptr;
        }

        qInfo("OAuthClient: emitting granted (accountId='%s')",
              qUtf8Printable(t.accountId));
        emit granted();
    });
}

bool OAuthClient::refreshIfNeededLocked() {
    // tokenMutex held on entry. We RELEASE it across the event loop so other
    // threads can read tokens (e.g. concurrent batched requests). A second
    // refresh racing this one would be wasteful but not incorrect — both end
    // up writing the latest server-issued access_token.
    if (!d_->tokens.valid()) return false;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (d_->tokens.expiresAtUnix - now > 60) return true;

    const QString clientId     = d_->config->clientId();
    const QString clientSecret = d_->config->clientSecret();
    const QString refreshToken = d_->tokens.refreshToken;

    d_->tokenMutex.unlock();
    QByteArray body;
    QNetworkReply::NetworkError netErr;
    QString netErrText;
    {
        QNetworkRequest req{QUrl(QLatin1String(kTokenEndpoint))};
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));

        QUrlQuery q;
        q.addQueryItem(QStringLiteral("client_id"),     clientId);
        q.addQueryItem(QStringLiteral("client_secret"), clientSecret);
        q.addQueryItem(QStringLiteral("grant_type"),    QStringLiteral("refresh_token"));
        q.addQueryItem(QStringLiteral("refresh_token"), refreshToken);

        QEventLoop loop;
        auto* reply = d_->nam->post(req, q.toString(QUrl::FullyEncoded).toUtf8());
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        body       = reply->readAll();
        netErr     = reply->error();
        netErrText = reply->errorString();
        reply->deleteLater();
    }
    d_->tokenMutex.lock();

    if (netErr != QNetworkReply::NoError) {
        qWarning("Token refresh failed: %s", qUtf8Printable(netErrText));
        return false;
    }
    const auto o = QJsonDocument::fromJson(body).object();
    d_->tokens.accessToken   = o.value(QStringLiteral("access_token")).toString();
    d_->tokens.expiresAtUnix = QDateTime::currentSecsSinceEpoch()
                             + static_cast<qint64>(o.value(QStringLiteral("expires_in")).toDouble());
    // Refresh tokens are sticky; Google only re-issues them on consent.

    auto persisted = d_->tokens;
    persisted.accountId = d_->accountId;   // stamp from the bound id
    d_->tokens.accountId = d_->accountId;
    if (!persisted.accountId.isEmpty()) {
        d_->store->save(persisted, [](bool ok, QString saveErr) {
            if (!ok) qWarning("TokenStore::save (refresh) failed: %s",
                              qUtf8Printable(saveErr));
        });
    }
    return true;
}

QString OAuthClient::accessTokenBlocking() {
    QMutexLocker lock(&d_->tokenMutex);
    if (!refreshIfNeededLocked()) return {};
    return d_->tokens.accessToken;
}

void OAuthClient::signOut() {
    TokenStore::Tokens copy;
    QString accountId;
    {
        QMutexLocker lock(&d_->tokenMutex);
        copy = d_->tokens;
        accountId = d_->accountId;
        d_->tokens = {};
    }
    if (!copy.refreshToken.isEmpty()) {
        QUrl u{QLatin1String(kRevokeEndpoint)};
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("token"), copy.refreshToken);
        u.setQuery(q);
        auto* reply = d_->nam->post(QNetworkRequest(u), QByteArray());
        connect(reply, &QNetworkReply::finished, reply, [reply] {
            if (reply->error() != QNetworkReply::NoError) {
                qWarning("Refresh-token revoke failed: %s — token may still "
                         "be valid on Google. Visit "
                         "https://myaccount.google.com/permissions to revoke "
                         "manually.",
                         qUtf8Printable(reply->errorString()));
            }
            reply->deleteLater();
        });
    }
    if (accountId.isEmpty()) {
        // Unbound client never had a keychain slot — just emit so
        // callers wired to signedOut still wake up.
        emit signedOut();
        return;
    }
    d_->store->erase(accountId,
        [this](bool, QString) { emit signedOut(); });
}

}  // namespace fc::auth
