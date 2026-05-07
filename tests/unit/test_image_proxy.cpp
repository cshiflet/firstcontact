#include "util/ImageProxy.h"

#include <QObject>
#include <QtTest>

class TestImageProxy : public QObject {
    Q_OBJECT
private:
    static QString kProxy() { return QStringLiteral("https://wsrv.nl/?url={url}"); }

private slots:
    void rewritesAbsoluteHttpsImg() {
        const QString in  = QStringLiteral(
            "<p>hi</p><img src=\"https://m.example.com/a.png\" alt=\"\">");
        const QString out = fc::util::rewriteImagesForBrowser(in, kProxy(),
                                                                /*strip=*/false);
        QVERIFY(out.contains(QStringLiteral("https://wsrv.nl/?url=")));
        QVERIFY(out.contains(QStringLiteral("https%3A//m.example.com/a.png"))
              || out.contains(QStringLiteral("https%3A%2F%2Fm.example.com%2Fa.png")));
        // Original src must be replaced, not appended.
        QVERIFY(!out.contains(QStringLiteral("src=\"https://m.example.com/")));
    }

    void rewritesHttpImg() {
        const QString in  = QStringLiteral(
            "<img src=\"http://insecure.example.com/x.gif\">");
        const QString out = fc::util::rewriteImagesForBrowser(in, kProxy(), false);
        QVERIFY(out.contains(QStringLiteral("https://wsrv.nl/?url=")));
        QVERIFY(!out.contains(QStringLiteral("\"http://insecure")));
    }

    void leavesDataUrls() {
        const QString in  = QStringLiteral(
            "<img src=\"data:image/png;base64,AAAA\">");
        const QString out = fc::util::rewriteImagesForBrowser(in, kProxy(), false);
        QCOMPARE(out, in);
    }

    void leavesCidUrls() {
        const QString in  = QStringLiteral(
            "<img src=\"cid:abc123@example\">");
        const QString out = fc::util::rewriteImagesForBrowser(in, kProxy(), false);
        QCOMPARE(out, in);
    }

    void leavesRelativeUrls() {
        const QString in  = QStringLiteral(
            "<img src=\"/local/path/x.png\">");
        const QString out = fc::util::rewriteImagesForBrowser(in, kProxy(), false);
        QCOMPARE(out, in);
    }

    void rewritesSrcset() {
        const QString in  = QStringLiteral(
            "<img src=\"https://a.example/1.png\" "
            "srcset=\"https://a.example/1x.png 1x, "
                    "https://a.example/2x.png 2x\">");
        const QString out = fc::util::rewriteImagesForBrowser(in, kProxy(), false);
        // Both srcset entries should be proxied.
        const int proxied = out.count(QStringLiteral("https://wsrv.nl/?url="));
        QCOMPARE(proxied, 3);   // src + two srcset entries
        QVERIFY(out.contains(QStringLiteral(" 1x")));
        QVERIFY(out.contains(QStringLiteral(" 2x")));
    }

    void stripsTinyImageWhenAsked() {
        const QString in = QStringLiteral(
            "<p>before</p>"
            "<img src=\"https://t.example/p.gif\" width=\"1\" height=\"1\">"
            "<p>after</p>");
        const QString out = fc::util::rewriteImagesForBrowser(
            in, kProxy(), /*strip=*/true);
        QVERIFY(!out.contains(QStringLiteral("<img")));
        QVERIFY(out.contains(QStringLiteral("before")));
        QVERIFY(out.contains(QStringLiteral("after")));
    }

    void stripsTinyImageInlineStyle() {
        const QString in = QStringLiteral(
            "<img src=\"https://t.example/p.gif\" "
            "style=\"width:1px; height:1px;\">");
        const QString out = fc::util::rewriteImagesForBrowser(in, kProxy(),
                                                                /*strip=*/true);
        QVERIFY(!out.contains(QStringLiteral("<img")));
    }

    void keepsNonTinyImagesWhenStripping() {
        const QString in = QStringLiteral(
            "<img src=\"https://m.example.com/photo.jpg\" "
            "width=\"600\" height=\"400\">");
        const QString out = fc::util::rewriteImagesForBrowser(in, kProxy(),
                                                                /*strip=*/true);
        QVERIFY(out.contains(QStringLiteral("<img")));
        QVERIFY(out.contains(QStringLiteral("https://wsrv.nl/?url=")));
    }

    void emptyProxyPatternLeavesUrlsAlone() {
        const QString in  = QStringLiteral(
            "<img src=\"https://m.example.com/a.png\">");
        const QString out = fc::util::rewriteImagesForBrowser(in, QString(),
                                                                /*strip=*/false);
        QCOMPARE(out, in);
    }

    void idempotentOnAlreadyProxied() {
        const QString once = fc::util::rewriteImagesForBrowser(
            QStringLiteral("<img src=\"https://m.example.com/a.png\">"),
            kProxy(), false);
        const QString twice = fc::util::rewriteImagesForBrowser(
            once, kProxy(), false);
        QCOMPARE(twice, once);
    }
};

QTEST_APPLESS_MAIN(TestImageProxy)
#include "test_image_proxy.moc"
