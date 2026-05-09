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

    void handlesGreaterThanInsideQuotedAttribute() {
        // Tag-end scanner used to use indexOf('>') which terminated
        // the tag at the first `>` even when it sat inside a quoted
        // attribute value. The malicious tail then leaked into the
        // output as escaped text — not exploitable, but visibly
        // garbage. Now the scanner respects `'`/`"` quoting.
        const auto r = fc::util::sanitizeHtml(
            QStringLiteral("<a title=\"a>b\" href=\"https://example.com/x\">link</a>"));
        QVERIFY( r.html.contains(QStringLiteral("href=\"https://example.com/x\"")));
        QVERIFY( r.html.contains(QStringLiteral(">link</a>")));
        // The tail of the original tag must NOT have leaked as text.
        QVERIFY(!r.html.contains(QStringLiteral("href=&quot;")));
        QVERIFY(!r.html.contains(QStringLiteral("&gt;link")));
    }

    void nestedDropTagDepthTrackedCorrectly() {
        // The drop-subtree state machine increments depth on a
        // matching open inside a drop region and decrements on each
        // close. A nested same-name pair must not prematurely
        // re-enable output.
        const auto r = fc::util::sanitizeHtml(
            QStringLiteral("<p>before</p>"
                           "<script>outer<script>inner</script>still-dropped"
                           "</script><p>after</p>"));
        QVERIFY( r.html.contains(QStringLiteral("before")));
        QVERIFY( r.html.contains(QStringLiteral("after")));
        QVERIFY(!r.html.contains(QStringLiteral("outer")));
        QVERIFY(!r.html.contains(QStringLiteral("inner")));
        QVERIFY(!r.html.contains(QStringLiteral("still-dropped")));
        QVERIFY(!r.html.contains(QStringLiteral("script")));
    }

    void doesNotSelfCloseDivLikeVoid() {
        // XHTML-style `<div/>` parses as an OPEN `<div>` in HTML5;
        // emitting it as `<div />` would leave an orphan opener.
        // Only true void elements (br, hr, img, col) get the
        // self-closing suffix.
        const auto r = fc::util::sanitizeHtml(
            QStringLiteral("<div/>hello"));
        QVERIFY( r.html.contains(QStringLiteral("<div>")));
        QVERIFY(!r.html.contains(QStringLiteral("<div />")));
        // br SHOULD still self-close.
        const auto r2 = fc::util::sanitizeHtml(QStringLiteral("a<br/>b"));
        QVERIFY( r2.html.contains(QStringLiteral("<br />")));
    }

    // Regression: a void <meta> tag in <head> must NOT eat the rest of
    // the document. Earlier code set dropDepth=1 on the open and waited
    // forever for </meta> — the body and everything inside it was
    // suppressed silently, rendering marketing emails blank.
    void voidDropTagDoesNotSwallowDocument() {
        const auto r = fc::util::sanitizeHtml(QStringLiteral(
            "<html><head><meta charset='utf-8'>"
            "<style>.foo{}</style></head>"
            "<body><p>visible content</p>"
            "<table><tr><td>nested</td></tr></table></body></html>"));
        QVERIFY(r.html.contains(QStringLiteral("visible content")));
        QVERIFY(r.html.contains(QStringLiteral("nested")));
        // Style block subtree still dropped (body's content lives
        // outside the dropped subtree, so it's preserved).
        QVERIFY(!r.html.contains(QStringLiteral(".foo")));
    }

    // The other void elements that appear in dropTags — link, base,
    // input, embed — also must not swallow the rest of the doc.
    void allVoidDropTagsAreNonSwallowing() {
        for (const auto* tag : {"<link rel='stylesheet'>",
                                "<base href='/foo'>",
                                "<input type='text'>",
                                "<embed src='x.swf'>"}) {
            const auto r = fc::util::sanitizeHtml(QStringLiteral(
                "<html><body>%1<p>after</p></body></html>").arg(QString::fromLatin1(tag)));
            QVERIFY2(r.html.contains(QStringLiteral("after")),
                     qPrintable(QStringLiteral("after-text disappeared with %1")
                                .arg(QString::fromLatin1(tag))));
        }
    }
};

QTEST_APPLESS_MAIN(TestHtmlSanitizer)
#include "test_html_sanitizer.moc"
