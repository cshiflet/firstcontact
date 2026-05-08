#include "util/Linkify.h"

#include <QObject>
#include <QtTest>

class TestLinkify : public QObject {
    Q_OBJECT
private slots:
    void wrapsHttpUrl() {
        const auto out = fc::util::linkifyPlainText(
            QStringLiteral("see https://example.com/x"));
        QVERIFY(out.contains(QStringLiteral("href=\"https://example.com/x\"")));
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

    // -------------------------------------------------------------- New behaviour

    void rendersLabeledLinkAsLabelOnly() {
        // baremail-terminal style: `text [https://url]` from Gmail's
        // text/plain converter becomes <a href="url" title="url">text</a>
        // — the URL is hidden behind a hover-tooltip / click instead of
        // dominating the prose.
        const auto out = fc::util::linkifyPlainText(
            QStringLiteral("Click here [https://example.com/x] now."));
        QVERIFY(out.contains(QStringLiteral(
            "<a href=\"https://example.com/x\" title=\"https://example.com/x\">"
            "Click here</a>")));
        // The bracketed URL must NOT also appear as visible text.
        QVERIFY(!out.contains(QStringLiteral("[https://example.com/x]")));
    }

    void labeledLinkInsideSentence() {
        const auto out = fc::util::linkifyPlainText(
            QStringLiteral("Read the docs [https://docs.example.com/page] for more."));
        QVERIFY(out.contains(QStringLiteral(
            "<a href=\"https://docs.example.com/page\" "
            "title=\"https://docs.example.com/page\">Read the docs</a>")));
    }

    void truncatesLongBareUrl() {
        const QString url = QStringLiteral(
            "https://example.com/very/long/path/that/keeps/going/"
            "and/going/until/it/exceeds/the/display/budget/leaf");
        QVERIFY(url.size() > 60);
        const auto out = fc::util::linkifyPlainText(url);
        // Click target stays the full URL.
        QVERIFY(out.contains(QStringLiteral("href=\"%1\"").arg(url)));
        // But the visible text gets the start…end truncation.
        QVERIFY(out.contains(QChar(0x2026)));   // ellipsis
        QVERIFY(!out.contains(QStringLiteral(">%1<").arg(url)));   // not visibly full
    }

    void rendersHtmlAnchorAsLabelOnly() {
        // Real-world bug: some senders dump literal HTML anchors into
        // the text/plain alternative (UPS shipment notifications are
        // a recurring example). Without a dedicated pre-pass the
        // linkifier escapes the angle brackets and renders the whole
        // tag as text noise.
        const QString in = QStringLiteral(
            "Track your shipment: <a href=\"https://wwwapps.ups.com/WebTracking/track?n=A&amp;tn=1Z9\" "
            "target=\"_blank\" style=\"color:#fff;\">Track shipment</a> today.");
        const auto out = fc::util::linkifyPlainText(in);
        QVERIFY(out.contains(QStringLiteral(
            "<a href=\"https://wwwapps.ups.com/WebTracking/track?n=A&amp;tn=1Z9\" "
            "title=\"https://wwwapps.ups.com/WebTracking/track?n=A&amp;tn=1Z9\">"
            "Track shipment</a>")));
        // Raw HTML must NOT survive in the rendered output.
        QVERIFY(!out.contains(QStringLiteral("&lt;a ")));
        QVERIFY(!out.contains(QStringLiteral("target=")));
        QVERIFY(!out.contains(QStringLiteral("&lt;/a&gt;")));
    }

    void rendersMarkdownLinkAsLabelOnly() {
        // Some senders pre-convert HTML→markdown for the text/plain
        // alternative — Amazon transactional mail in particular emits
        // `[label](url)`. Without dedicated handling the bare-URL
        // pass would only linkify the URL inside the parens and leave
        // the markdown brackets as visible text.
        const auto out = fc::util::linkifyPlainText(
            QStringLiteral("[Privacy Notice](https://amazon.com/help/privacy) follows."));
        QVERIFY(out.contains(QStringLiteral(
            "<a href=\"https://amazon.com/help/privacy\" "
            "title=\"https://amazon.com/help/privacy\">Privacy Notice</a>")));
        // Markdown brackets / parens must NOT remain in the output.
        QVERIFY(!out.contains(QStringLiteral("[Privacy Notice]")));
        QVERIFY(!out.contains(QStringLiteral("(https://amazon.com")));
    }

    void doesNotDoubleWrapWhenLabelLooksLikeUrl() {
        // "https://a [https://b]" should leave each URL standalone — no
        // labeled-link match where the label is itself a URL.
        const auto out = fc::util::linkifyPlainText(
            QStringLiteral("https://a.test [https://b.test]"));
        // Both URLs ended up linkified (somewhere). The exact rendering
        // depends on the bracket trim, but neither URL should appear as
        // a label of the other.
        QVERIFY(out.contains(QStringLiteral("href=\"https://a.test\"")));
        QVERIFY(out.contains(QStringLiteral("href=\"https://b.test\"")));
    }

    // -------------------------------------------------------------- Entities

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
