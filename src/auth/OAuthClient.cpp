#include "OAuthClient.h"

#include "ClientConfig.h"
#include "util/Base64Url.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOAuth2AuthorizationCodeFlow>
#include <QOAuthHttpServerReplyHandler>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QStandardPaths>
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

// True if we're running inside WSL (Windows Subsystem for Linux). Cached.
bool isWsl() {
    static bool checked = false;
    static bool result = false;
    if (!checked) {
        checked = true;
        if (!QProcessEnvironment::systemEnvironment()
                 .value(QStringLiteral("WSL_DISTRO_NAME")).isEmpty()) {
            result = true;
        } else {
            QFile f(QStringLiteral("/proc/version"));
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray v = f.readAll();
                result = v.contains("Microsoft") || v.contains("microsoft")
                      || v.contains("WSL");
            }
        }
        if (result) qInfo("OAuth: WSL environment detected");
    }
    return result;
}

// Try increasingly specific strategies to launch the system browser. Returns
// true on the first one that successfully starts a child process. Logs the
// strategy used (or all the failures) for diagnostics.
//
// Why we don't trust QDesktopServices::openUrl alone: on Linux it relies on
// xdg-open / xdg-utils, which is missing from minimal images, and there have
// been long-running issues (Qt bug 90984 et al.) where the call silently
// returns false when xdg-open is present but can't reach a snap-confined
// browser. WSL is its own can of worms — there's no Linux browser at all,
// so we have to bridge to the Windows host via wslview / cmd.exe.
bool launchBrowser(const QUrl& url) {
    const QString u = url.toString();

    auto tryRun = [&](const QString& label, const QString& exe,
                      const QStringList& args) {
        if (QStandardPaths::findExecutable(exe).isEmpty()) return false;
        if (!QProcess::startDetached(exe, args)) return false;
        qInfo("OAuth: launched browser via '%s'", qUtf8Printable(label));
        return true;
    };

    // On WSL the Linux side typically has no browser at all — bridge to the
    // Windows host before falling back to Linux launchers. The redirect
    // back to http://127.0.0.1:<port>/ works because WSL2 forwards
    // localhost between Windows and Linux automatically.
    if (isWsl()) {
        // wslview (from the wslu package) is the canonical WSL equivalent
        // of xdg-open and quotes URLs correctly.
        if (tryRun("wslview", "wslview", {u})) return true;
        // cmd.exe is always available via /mnt/c. The empty-string title
        // arg is needed so `start` doesn't interpret the URL as a title.
        if (tryRun("cmd.exe",        "cmd.exe",
                   {"/c", "start", "", u})) return true;
        if (tryRun("powershell.exe", "powershell.exe",
                   {"-NoProfile", "-Command",
                    QStringLiteral("Start-Process '%1'").arg(u)})) return true;
        // Fall through: someone might have installed a Linux browser even
        // on WSL, and the user's PATH still has xdg-open etc.
    }

    if (QDesktopServices::openUrl(url)) {
        qInfo("OAuth: launched browser via QDesktopServices");
        return true;
    }

    const QString browserEnv =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("BROWSER"));
    if (!browserEnv.isEmpty()
        && tryRun("$BROWSER", browserEnv, {u})) return true;

    if (tryRun("xdg-open",         "xdg-open",         {u})) return true;
    if (tryRun("sensible-browser", "sensible-browser", {u})) return true;
    if (tryRun("gio open",         "gio",              {"open", u})) return true;
    if (tryRun("kde-open5",        "kde-open5",        {u})) return true;
    if (tryRun("gnome-open",       "gnome-open",       {u})) return true;

    for (const QString& exe : {QStringLiteral("firefox"),
                                QStringLiteral("google-chrome"),
                                QStringLiteral("chromium"),
                                QStringLiteral("chromium-browser"),
                                QStringLiteral("brave-browser"),
                                QStringLiteral("microsoft-edge")}) {
        if (tryRun(exe, exe, {u})) return true;
    }

    qWarning("OAuth: every browser launch strategy failed — "
             "the user will need to copy the URL manually");
    return false;
}

}  // namespace

struct OAuthClient::Impl {
    ClientConfig* config = nullptr;
    TokenStore*   store  = nullptr;

    QOAuth2AuthorizationCodeFlow*   flow    = nullptr;
    QOAuthHttpServerReplyHandler*   handler = nullptr;
    QNetworkAccessManager*          nam     = nullptr;

    QByteArray codeVerifier;

    mutable QMutex tokenMutex;
    TokenStore::Tokens tokens;
};

OAuthClient::OAuthClient(ClientConfig* config, TokenStore* store, QObject* parent)
    : QObject(parent), d_(new Impl) {
    d_->config = config;
    d_->store  = store;
    d_->nam    = new QNetworkAccessManager(this);

    hydrateFromStore();
}

OAuthClient::~OAuthClient() { delete d_; }

void OAuthClient::hydrateFromStore() {
    d_->store->load([this](bool ok, TokenStore::Tokens t, QString err) {
        if (!ok) {
            qWarning("TokenStore::load failed: %s", qUtf8Printable(err));
            return;
        }
        QMutexLocker lock(&d_->tokenMutex);
        d_->tokens = std::move(t);
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

void OAuthClient::wireFlow() {
    if (d_->flow) return;

    d_->codeVerifier = makeCodeVerifier();
    const QByteArray challenge = makeCodeChallenge(d_->codeVerifier);

    d_->flow    = new QOAuth2AuthorizationCodeFlow(d_->nam, this);
    d_->handler = new QOAuthHttpServerReplyHandler(0, this);  // ephemeral port
    if (!d_->handler->isListening()) {
        qWarning("OAuth loopback handler failed to bind a port — "
                 "the redirect after Google consent will time out");
    } else {
        qInfo("OAuth loopback handler listening on %s",
              qUtf8Printable(d_->handler->callback()));
    }

    d_->flow->setAuthorizationUrl(QUrl(QLatin1String(kAuthEndpoint)));
    d_->flow->setAccessTokenUrl(QUrl(QLatin1String(kTokenEndpoint)));
    d_->flow->setClientIdentifier(d_->config->clientId());
    d_->flow->setScope(kScopeString());
    d_->flow->setReplyHandler(d_->handler);

    // Manual PKCE injection (Qt 6.6 added setPkceMethod; we target 6.4+).
    d_->flow->setModifyParametersFunction(
        [verifier = d_->codeVerifier, challenge](
            QAbstractOAuth::Stage stage, QMultiMap<QString, QVariant>* params) {
            switch (stage) {
                case QAbstractOAuth::Stage::RequestingAuthorization:
                    params->insert(QStringLiteral("code_challenge"),
                                   QString::fromLatin1(challenge));
                    params->insert(QStringLiteral("code_challenge_method"),
                                   QStringLiteral("S256"));
                    params->insert(QStringLiteral("access_type"),
                                   QStringLiteral("offline"));
                    params->insert(QStringLiteral("prompt"),
                                   QStringLiteral("consent"));
                    break;
                case QAbstractOAuth::Stage::RequestingAccessToken:
                    params->insert(QStringLiteral("code_verifier"),
                                   QString::fromLatin1(verifier));
                    break;
                default:
                    break;
            }
        });

    connect(d_->flow, &QOAuth2AuthorizationCodeFlow::authorizeWithBrowser,
            this, [this](const QUrl& url) {
                qInfo("OAuth authorize URL: %s", qUtf8Printable(url.toString()));
                const bool opened = launchBrowser(url);
                emit browserAuthRequested(url, opened);
            });

    connect(d_->flow, &QOAuth2AuthorizationCodeFlow::granted, this, [this] {
        persistFromFlow();
        emit granted();
    });

    connect(d_->flow, &QOAuth2AuthorizationCodeFlow::error, this,
            [this](const QString& err, const QString& desc, const QUrl&) {
                emit failed(err + QStringLiteral(": ") + desc);
            });
}

void OAuthClient::authorize() {
    if (!d_->config->isConfigured()) {
        emit failed(tr("OAuth client_id is not configured. Run the setup wizard."));
        return;
    }
    wireFlow();
    d_->flow->grant();
}

void OAuthClient::persistFromFlow() {
    TokenStore::Tokens t;
    t.accessToken   = d_->flow->token();
    t.refreshToken  = d_->flow->refreshToken();
    t.expiresAtUnix = d_->flow->expirationAt().toSecsSinceEpoch();
    // Email is filled in by api/GmailClient::getProfile() on first sync; keep
    // whatever we already had until then.
    {
        QMutexLocker lock(&d_->tokenMutex);
        if (t.accountEmail.isEmpty()) t.accountEmail = d_->tokens.accountEmail;
        d_->tokens = t;
    }
    d_->store->save(t, [](bool ok, QString err) {
        if (!ok) qWarning("TokenStore::save failed: %s", qUtf8Printable(err));
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
    d_->store->save(persisted, [](bool ok, QString saveErr) {
        if (!ok) qWarning("TokenStore::save (refresh) failed: %s",
                          qUtf8Printable(saveErr));
    });
    return true;
}

QString OAuthClient::accessTokenBlocking() {
    QMutexLocker lock(&d_->tokenMutex);
    if (!refreshIfNeededLocked()) return {};
    return d_->tokens.accessToken;
}

void OAuthClient::signOut() {
    TokenStore::Tokens copy;
    {
        QMutexLocker lock(&d_->tokenMutex);
        copy = d_->tokens;
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
    d_->store->erase([this](bool, QString) { emit signedOut(); });
}

}  // namespace fc::auth
