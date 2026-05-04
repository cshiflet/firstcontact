#include "util/Linkify.h"

#include <QObject>
#include <QtTest>

class TestLinkify : public QObject {
    Q_OBJECT
private slots:
    void wrapsHttpUrl() {
        const auto out = fc::util::linkifyPlainText(
            QStringLiteral("see https://example.com/x"));
        QVERIFY(out.contains(QStringLiteral("<a href=\"https://example.com/x\">")));
    }

    void trimsTrailingPunctuation() {
        const auto out = fc::util::linkifyPlainText(
            QStringLiteral("see https://example.com/x."));
        QVERIFY(out.contains(QStringLiteral("href=\"https://example.com/x\"")));
        QVERIFY(out.endsWith(QStringLiteral(".")));
    }

    void wrapsBareEmail() {
        const auto out = fc::util::linkifyPlainText(
            QStringLiteral("ping a@b.test for details"));
        QVERIFY(out.contains(QStringLiteral("<a href=\"mailto:a@b.test\">")));
    }

    void decodesNumericEntityReferences() {
        // Real-world bug: Gmail sometimes serves text/plain bodies that
        // still contain HTML entity references, e.g. "Lorem &#847; ipsum".
        // Without decoding the user sees the literal "&#847;" string.
        const auto decimal = fc::util::linkifyPlainText(
            QStringLiteral("Lorem &#847; ipsum"));
        QVERIFY( decimal.contains(QChar(847)));
        QVERIFY(!decimal.contains(QStringLiteral("&#847;")));
        QVERIFY(!decimal.contains(QStringLiteral("&amp;#847;")));

        const auto hex = fc::util::linkifyPlainText(
            QStringLiteral("Lorem &#x34F; ipsum"));
        QVERIFY(hex.contains(QChar(0x34F)));
    }

    void decodesNamedEntityReferences() {
        const auto out = fc::util::linkifyPlainText(
            QStringLiteral("Tom&apos;s &mdash; he said &ldquo;hi&rdquo;"));
        QVERIFY(out.contains(QChar('\'')));
        QVERIFY(out.contains(QChar(0x2014)));   // em-dash
        QVERIFY(out.contains(QChar(0x201C)));   // left double quote
        QVERIFY(out.contains(QChar(0x201D)));   // right double quote
    }

    void leavesUnknownEntitiesAlone() {
        // Bare ampersand and a "fake" entity-looking sequence should not be
        // munged — they get HTML-escaped for safe display.
        const auto out = fc::util::linkifyPlainText(
            QStringLiteral("a & b &nope; c"));
        QVERIFY(out.contains(QStringLiteral("a &amp; b &amp;nope; c")));
    }
};

QTEST_APPLESS_MAIN(TestLinkify)
#include "test_linkify.moc"
