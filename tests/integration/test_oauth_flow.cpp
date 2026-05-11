// Drives OAuthClient::authorize() to verify the authorization URL it
// emits to the user's browser conforms to RFC 6749 + RFC 7636 (PKCE)
// and the Google Identity OAuth desktop-app conventions:
//   - response_type=code
//   - code_challenge_method=S256
//   - code_challenge length = 43 (base64url-encoded SHA-256, no pad)
//   - state present
//   - scope contains the gmail.* set
//   - redirect_uri points at the loopback handler
//
// We do NOT exercise the token-exchange path here — that requires a
// real authorization code and a fake authz server. The full end-to-end
// test against a QHttpServer-based fake authz endpoint is the next
// scenario to add (TODO list at the bottom of this file).

#include "auth/ClientConfig.h"
#include "auth/OAuthClient.h"
#include "auth/TokenStore.h"

#include <QCoreApplication>
#include <QObject>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QUrlQuery>
#include <QtTest>

class TestOAuthFlow : public QObject {
    Q_OBJECT

private:
    QTemporaryDir tmp_;

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("FirstContactTest"));
        QCoreApplication::setApplicationName(QStringLiteral("FirstContactTest"));
        QStandardPaths::setTestModeEnabled(true);

        // Seed a client_id + client_secret in QSettings so ClientConfig
        // reports isConfigured() == true. The values are arbitrary —
        // no real Google project is contacted in this test.
        QSettings s;
        s.setValue(QStringLiteral("oauth/client_id"),
                   QStringLiteral("fake-id.apps.googleusercontent.com"));
        s.setValue(QStringLiteral("oauth/client_secret"),
                   QStringLiteral("fake-secret"));
        s.sync();
    }

    void authorizeUrlConformsToPkce() {
        fc::auth::ClientConfig config;
        QVERIFY(config.isConfigured());

        fc::auth::TokenStore store;
        fc::auth::OAuthClient oauth(&config, &store, /*accountId=*/QString());

        // browserAuthRequested fires from inside authorize() once the URL
        // has been built and the loopback handler is listening. We catch
        // it via QSignalSpy and parse the URL.
        QSignalSpy spy(&oauth, &fc::auth::OAuthClient::browserAuthRequested);
        oauth.authorize();
        // browserAuthRequested may emit synchronously (URL built before
        // browser launch) or after the launch attempt; wait briefly.
        QVERIFY(spy.wait(2000) || !spy.isEmpty());
        QVERIFY(!spy.isEmpty());

        const QUrl url = spy.last().at(0).toUrl();
        QVERIFY(url.isValid());
        QCOMPARE(url.host(), QStringLiteral("accounts.google.com"));

        const QUrlQuery q(url);
        QCOMPARE(q.queryItemValue(QStringLiteral("response_type")),
                 QStringLiteral("code"));
        QCOMPARE(q.queryItemValue(QStringLiteral("code_challenge_method")),
                 QStringLiteral("S256"));

        const QString challenge = q.queryItemValue(QStringLiteral("code_challenge"));
        // base64url-encoded SHA-256 of the verifier — exactly 43 chars,
        // unpadded, drawn from [A-Za-z0-9_-].
        QCOMPARE(challenge.size(), 43);
        for (QChar c : challenge) {
            const bool ok = c.isLetterOrNumber() || c == '-' || c == '_';
            QVERIFY2(ok, qPrintable(QStringLiteral("non-base64url char: %1").arg(c)));
        }

        const QString state = q.queryItemValue(QStringLiteral("state"));
        QVERIFY(!state.isEmpty());

        const QString scope = q.queryItemValue(QStringLiteral("scope"));
        QVERIFY(scope.contains(QStringLiteral("gmail.readonly")));
        QVERIFY(scope.contains(QStringLiteral("gmail.send")));
        QVERIFY(scope.contains(QStringLiteral("gmail.modify")));
        QVERIFY(scope.contains(QStringLiteral("gmail.compose")));
        QVERIFY(scope.contains(QStringLiteral("gmail.labels")));

        const QString redirect = q.queryItemValue(QStringLiteral("redirect_uri"));
        // QOAuthHttpServerReplyHandler binds 127.0.0.1 (RFC 8252 preferred).
        QVERIFY(redirect.startsWith(QStringLiteral("http://127.0.0.1:"))
                || redirect.startsWith(QStringLiteral("http://localhost:")));

        QCOMPARE(q.queryItemValue(QStringLiteral("client_id")),
                 QStringLiteral("fake-id.apps.googleusercontent.com"));
    }

    void authorizeFailsWhenClientIdMissing() {
        QSettings s;
        s.remove(QStringLiteral("oauth/client_id"));
        s.remove(QStringLiteral("oauth/client_secret"));
        s.sync();

        fc::auth::ClientConfig config;
        QVERIFY(!config.isConfigured());

        fc::auth::TokenStore store;
        fc::auth::OAuthClient oauth(&config, &store, /*accountId=*/QString());
        QSignalSpy failed(&oauth, &fc::auth::OAuthClient::failed);
        oauth.authorize();
        QVERIFY(failed.wait(1000) || !failed.isEmpty());
        QVERIFY(!failed.isEmpty());
    }
};

// TODO scenarios to add (each requires more scaffolding):
//   - end-to-end PKCE: spin up a QHttpServer fake of accounts.google.com
//     + oauth2.googleapis.com/token, drive a full code-grant flow,
//     verify TokenStore receives valid tokens, isAuthorized()→true.
//   - state mismatch: feed back a callback with state != what was sent;
//     verify failed() fires with a recognisable reason and tokens stay
//     unset.
//   - 401 mid-session: with a token loaded, intercept a Gmail API call
//     to return 401, verify silent refresh → re-attempt → success.
//   - revoke: signOut() should call revoke endpoint and clear keychain.
//
// All four need a fake authz server; that lives in a separate helper
// class once test_oauth_flow grows beyond the URL-shape checks above.

QTEST_MAIN(TestOAuthFlow)
#include "test_oauth_flow.moc"
