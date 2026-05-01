#include "util/HtmlSanitizer.h"

#include <QObject>
#include <QtTest>

class TestHtmlSanitizer : public QObject {
    Q_OBJECT
private slots:
    void dropsScriptAndStyleSubtree() {
        const auto r = fc::util::sanitizeHtml(
            QStringLiteral("<p>x</p><script>alert(1)</script>"
                           "<style>body{}</style><p>y</p>"));
        QVERIFY(!r.html.contains(QStringLiteral("alert")));
        QVERIFY(!r.html.contains(QStringLiteral("script")));
        QVERIFY(!r.html.contains(QStringLiteral("style")));
        QVERIFY( r.html.contains(QStringLiteral("x")));
        QVERIFY( r.html.contains(QStringLiteral("y")));
    }

    void stripsEventHandlers() {
        const auto r = fc::util::sanitizeHtml(
            QStringLiteral("<a href='https://example.com' onclick='evil()'>x</a>"));
        QVERIFY( r.html.contains(QStringLiteral("href=\"https://example.com\"")));
        QVERIFY(!r.html.contains(QStringLiteral("onclick")));
        QVERIFY(!r.html.contains(QStringLiteral("evil")));
    }

    void blocksJavascriptUrls() {
        const auto r = fc::util::sanitizeHtml(
            QStringLiteral("<a href='javascript:alert(1)'>click</a>"));
        QVERIFY(!r.html.contains(QStringLiteral("javascript")));
        QVERIFY( r.html.contains(QStringLiteral("click")));   // text preserved
    }

    void blocksDataUrlsInLinks() {
        const auto r = fc::util::sanitizeHtml(
            QStringLiteral("<a href='data:text/html,<script>1</script>'>x</a>"));
        QVERIFY(!r.html.contains(QStringLiteral("data:")));
    }

    void blocksRemoteImagesByDefault() {
        const auto r = fc::util::sanitizeHtml(
            QStringLiteral("<img src='https://tracker.test/1px.png' alt='x'>"));
        QVERIFY( r.remoteImagesBlocked);
        QVERIFY(!r.html.contains(QStringLiteral("tracker.test")));
    }

    void allowsRemoteImagesWhenOptedIn() {
        fc::util::SanitizeOptions opt; opt.allowRemoteImages = true;
        const auto r = fc::util::sanitizeHtml(
            QStringLiteral("<img src='https://example.com/p.png'>"), opt);
        QVERIFY(!r.remoteImagesBlocked);
        QVERIFY( r.html.contains(QStringLiteral("example.com/p.png")));
    }

    void dropsIframeAndForm() {
        const auto r = fc::util::sanitizeHtml(
            QStringLiteral("<p>a</p><iframe src='https://x.test'></iframe>"
                           "<form action='x'>nope</form><p>b</p>"));
        QVERIFY(!r.html.contains(QStringLiteral("iframe")));
        QVERIFY(!r.html.contains(QStringLiteral("form")));
        QVERIFY(!r.html.contains(QStringLiteral("nope")));
    }
};

QTEST_APPLESS_MAIN(TestHtmlSanitizer)
#include "test_html_sanitizer.moc"
