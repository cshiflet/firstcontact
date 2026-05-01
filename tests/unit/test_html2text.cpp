#include "util/Html2Text.h"

#include <QObject>
#include <QtTest>

class TestHtml2Text : public QObject {
    Q_OBJECT
private slots:
    void stripsTagsAndDecodesEntities() {
        const auto out = fc::util::html2text(
            QStringLiteral("<p>hello&nbsp;<b>world</b>&amp;more</p>"));
        // &nbsp; correctly decodes to U+00A0 (no-break space).
        QCOMPARE(out, QString(QStringLiteral("hello")) + QChar(0x00A0)
                       + QStringLiteral("world&more"));
    }

    void dropsScripts() {
        const auto out = fc::util::html2text(
            QStringLiteral("<p>x</p><script>alert(1)</script><p>y</p>"));
        QVERIFY(!out.contains(QStringLiteral("alert")));
        QVERIFY(out.contains(QStringLiteral("x")));
        QVERIFY(out.contains(QStringLiteral("y")));
    }

    void preservesParagraphBreaks() {
        const auto out = fc::util::html2text(
            QStringLiteral("<p>one</p><p>two</p>"));
        QVERIFY(out.contains(QStringLiteral("one")));
        QVERIFY(out.contains(QStringLiteral("two")));
        QVERIFY(out.contains(QChar('\n')));
    }
};

QTEST_APPLESS_MAIN(TestHtml2Text)
#include "test_html2text.moc"
