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
};

QTEST_APPLESS_MAIN(TestMessageParser)
#include "test_message_parser.moc"
