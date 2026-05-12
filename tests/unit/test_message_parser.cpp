#include "api/MessageParser.h"
#include "util/Base64Url.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QtTest>

class TestMessageParser : public QObject {
    Q_OBJECT
private slots:
    void parsesHeadersAndPlainBody() {
        QJsonObject body;
        body.insert(QStringLiteral("data"),
                    QString::fromLatin1(fc::util::base64UrlEncode("hello world")));
        body.insert(QStringLiteral("size"), 11);

        QJsonObject part;
        part.insert(QStringLiteral("mimeType"), QStringLiteral("text/plain"));
        part.insert(QStringLiteral("body"), body);

        QJsonArray headers;
        auto h = [&headers](const char* n, const QString& v) {
            QJsonObject o; o.insert("name", n); o.insert("value", v);
            headers.append(o);
        };
        h("Subject", QStringLiteral("hi"));
        h("From",    QStringLiteral("Chris <c@x.test>"));
        h("To",      QStringLiteral("a@x.test, b@x.test"));

        QJsonObject payload;
        payload.insert(QStringLiteral("mimeType"), QStringLiteral("text/plain"));
        payload.insert(QStringLiteral("headers"), headers);
        payload.insert(QStringLiteral("body"), body);
        payload.insert(QStringLiteral("parts"), QJsonArray{part});

        QJsonObject g;
        g.insert(QStringLiteral("id"),           QStringLiteral("m1"));
        g.insert(QStringLiteral("threadId"),     QStringLiteral("t1"));
        g.insert(QStringLiteral("internalDate"), QStringLiteral("1700000000000"));
        g.insert(QStringLiteral("labelIds"),
                 QJsonArray{QStringLiteral("INBOX"), QStringLiteral("UNREAD")});
        g.insert(QStringLiteral("payload"), payload);

        const auto m = fc::api::MessageParser::parse(g);
        QCOMPARE(m.id, QStringLiteral("m1"));
        QCOMPARE(m.subject, QStringLiteral("hi"));
        QCOMPARE(m.fromName, QStringLiteral("Chris"));
        QCOMPARE(m.fromAddr, QStringLiteral("c@x.test"));
        QCOMPARE(m.toAddrs.size(), 2);
        QCOMPARE(m.bodyText, QStringLiteral("hello world"));
        QVERIFY(m.isUnread);
    }
    void splitAddressListRespectsQuotedComma() {
        // Real-world bug: `"Smith, John" <s@x.test>, b@y.test` was
        // splitting into three pieces because the comma inside the
        // quoted display name was treated like a separator. Build a
        // minimal payload routed through MessageParser to exercise
        // splitAddressList without exposing it directly.
        QJsonObject body;
        body.insert(QStringLiteral("data"),
                     QString::fromLatin1(fc::util::base64UrlEncode(
                         QByteArrayLiteral("body"))));
        body.insert(QStringLiteral("size"), 4);

        QJsonArray headers;
        auto h = [&](const QString& n, const QString& v) {
            QJsonObject o;
            o.insert(QStringLiteral("name"), n);
            o.insert(QStringLiteral("value"), v);
            headers.append(o);
        };
        h("From", QStringLiteral("Sender <s@x.test>"));
        h("Subject", QStringLiteral("hi"));
        h("To",
          QStringLiteral(R"("Smith, John" <john@x.test>, b@y.test)"));

        QJsonObject payload;
        payload.insert(QStringLiteral("mimeType"), QStringLiteral("text/plain"));
        payload.insert(QStringLiteral("headers"), headers);
        payload.insert(QStringLiteral("body"), body);

        QJsonObject g;
        g.insert(QStringLiteral("id"), QStringLiteral("m"));
        g.insert(QStringLiteral("threadId"), QStringLiteral("t"));
        g.insert(QStringLiteral("internalDate"), QStringLiteral("0"));
        g.insert(QStringLiteral("payload"), payload);

        const auto m = fc::api::MessageParser::parse(g);
        QCOMPARE(m.toAddrs.size(), 2);
        QVERIFY(m.toAddrs[0].contains(QStringLiteral("Smith, John")));
        QVERIFY(m.toAddrs[0].contains(QStringLiteral("john@x.test")));
        QCOMPARE(m.toAddrs[1], QStringLiteral("b@y.test"));
    }

    // format=metadata responses come back without any body data, just
    // headers + labels + snippet. MessageParser must yield a Message
    // with empty body fields but populated metadata. This is the
    // shape the meta-first sync stores en masse.
    void emptyPayloadYieldsMetadataOnlyMessage() {
        QJsonArray headers;
        auto h = [&](const QString& n, const QString& v) {
            QJsonObject o;
            o.insert(QStringLiteral("name"), n);
            o.insert(QStringLiteral("value"), v);
            headers.append(o);
        };
        h(QStringLiteral("From"),    QStringLiteral("Dana <dana@x.test>"));
        h(QStringLiteral("Subject"), QStringLiteral("metadata only"));

        QJsonObject payload;
        // No mimeType, no body.data, no parts — mirrors what Gmail
        // returns for messages.get?format=metadata.
        payload.insert(QStringLiteral("headers"), headers);

        QJsonObject g;
        g.insert(QStringLiteral("id"),           QStringLiteral("m-meta"));
        g.insert(QStringLiteral("threadId"),     QStringLiteral("t-meta"));
        g.insert(QStringLiteral("internalDate"), QStringLiteral("1700000001000"));
        g.insert(QStringLiteral("snippet"),      QStringLiteral("preview"));
        g.insert(QStringLiteral("labelIds"),
                 QJsonArray{QStringLiteral("INBOX")});
        g.insert(QStringLiteral("payload"),      payload);

        const auto m = fc::api::MessageParser::parse(g);
        QCOMPARE(m.id, QStringLiteral("m-meta"));
        QCOMPARE(m.subject, QStringLiteral("metadata only"));
        QCOMPARE(m.fromAddr, QStringLiteral("dana@x.test"));
        QCOMPARE(m.snippet, QStringLiteral("preview"));
        QVERIFY(m.bodyText.isEmpty());
        QVERIFY(m.bodyHtml.isEmpty());
        QVERIFY(!m.bodyHtmlPresent);
        QVERIFY(m.labelIds.contains(QStringLiteral("INBOX")));
    }

    void splitAddressIgnoresAnglesInsideQuotes() {
        // Defensive: a display name that itself contains "< >" must
        // not steal the address slot. Without quote-aware parsing,
        // `<weird> <real@x.test>` would parse name="" addr="weird".
        QJsonObject body;
        body.insert(QStringLiteral("data"),
                     QString::fromLatin1(fc::util::base64UrlEncode(
                         QByteArrayLiteral("hi"))));
        body.insert(QStringLiteral("size"), 2);

        QJsonArray headers;
        auto h = [&](const QString& n, const QString& v) {
            QJsonObject o;
            o.insert(QStringLiteral("name"), n);
            o.insert(QStringLiteral("value"), v);
            headers.append(o);
        };
        h("From", QStringLiteral(R"("<weird>" <real@x.test>)"));

        QJsonObject payload;
        payload.insert(QStringLiteral("mimeType"), QStringLiteral("text/plain"));
        payload.insert(QStringLiteral("headers"), headers);
        payload.insert(QStringLiteral("body"), body);

        QJsonObject g;
        g.insert(QStringLiteral("id"), QStringLiteral("m"));
        g.insert(QStringLiteral("threadId"), QStringLiteral("t"));
        g.insert(QStringLiteral("internalDate"), QStringLiteral("0"));
        g.insert(QStringLiteral("payload"), payload);

        const auto m = fc::api::MessageParser::parse(g);
        QCOMPARE(m.fromAddr, QStringLiteral("real@x.test"));
        QCOMPARE(m.fromName, QStringLiteral("<weird>"));
    }
};

QTEST_APPLESS_MAIN(TestMessageParser)
#include "test_message_parser.moc"
