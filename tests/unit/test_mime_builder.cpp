#include "util/MimeBuilder.h"

#include <QObject>
#include <QtTest>

class TestMimeBuilder : public QObject {
    Q_OBJECT
private slots:
    void buildsMinimalMessage() {
        fc::util::OutgoingMessage m;
        m.fromAddr = QStringLiteral("a@x.test");
        m.fromName = QStringLiteral("Alice");
        m.to       = QStringLiteral("b@y.test").split(',');
        m.subject  = QStringLiteral("hello");
        m.bodyText = QStringLiteral("hi there\nline two");

        const QByteArray rfc = fc::util::MimeBuilder::build(m);
        QVERIFY(rfc.contains("From: Alice <a@x.test>"));
        QVERIFY(rfc.contains("To: b@y.test"));
        QVERIFY(rfc.contains("Subject: hello"));
        QVERIFY(rfc.contains("MIME-Version: 1.0"));
        QVERIFY(rfc.contains("Content-Type: text/plain; charset=UTF-8"));
        // Headers/body separated by CRLF CRLF.
        QVERIFY(rfc.contains("\r\n\r\nhi there\r\nline two"));
        // Message-ID is present and uses our domain heuristic.
        QVERIFY(rfc.contains("Message-ID: <"));
        QVERIFY(rfc.contains("@x.test>"));
    }

    void encodesNonAsciiSubject() {
        fc::util::OutgoingMessage m;
        m.fromAddr = QStringLiteral("a@x.test");
        m.to       = QStringLiteral("b@y.test").split(',');
        m.subject  = QString::fromUtf8("résumé — café");
        m.bodyText = QStringLiteral("body");

        const QByteArray rfc = fc::util::MimeBuilder::build(m);
        QVERIFY(rfc.contains("Subject: =?UTF-8?B?"));
    }

    void rejectsHeaderInjectionInRecipients() {
        // Attacker-controlled "name" or address tries to splice in Bcc by
        // embedding CRLF in a recipient. The injected text must collapse
        // onto the same physical line so it can't introduce a new header.
        fc::util::OutgoingMessage m;
        m.fromAddr = QStringLiteral("a@x.test");
        m.to       = {QStringLiteral("victim@y.test\r\nBcc: leak@z.test")};
        m.subject  = QStringLiteral("hi");
        m.bodyText = QStringLiteral("body");
        const QByteArray rfc = fc::util::MimeBuilder::build(m);
        const auto headers = rfc.left(rfc.indexOf("\r\n\r\n"));
        // No standalone Bcc header line.
        QVERIFY(!headers.contains("\r\nBcc:"));
        QVERIFY(!headers.startsWith("Bcc:"));
        // The To: header must be a single line.
        const int toStart = headers.indexOf("To:");
        QVERIFY(toStart >= 0);
        const int toEnd = headers.indexOf("\r\n", toStart);
        QVERIFY(toEnd > toStart);
        const QByteArray toLine = headers.mid(toStart, toEnd - toStart);
        QVERIFY(!toLine.contains('\r'));
        QVERIFY(!toLine.contains('\n'));
    }

    void rejectsHeaderInjectionInSubject() {
        fc::util::OutgoingMessage m;
        m.fromAddr = QStringLiteral("a@x.test");
        m.to       = {QStringLiteral("b@y.test")};
        m.subject  = QStringLiteral("hi\r\nBcc: leak@z.test\r\nX-Pwn: yes");
        m.bodyText = QStringLiteral("body");
        const QByteArray rfc = fc::util::MimeBuilder::build(m);
        const auto headers = rfc.left(rfc.indexOf("\r\n\r\n"));
        QVERIFY(!headers.contains("\r\nBcc:"));
        QVERIFY(!headers.contains("\r\nX-Pwn:"));
    }

    void buildsReplyHeaders() {
        fc::util::OutgoingMessage m;
        m.fromAddr        = QStringLiteral("a@x.test");
        m.to              = QStringLiteral("b@y.test").split(',');
        m.subject         = QStringLiteral("Re: hi");
        m.bodyText        = QStringLiteral("ok");
        m.rfc822InReplyTo = QStringLiteral("<orig@y.test>");
        m.rfc822References = {QStringLiteral("<grand@z.test>"),
                              QStringLiteral("<orig@y.test>")};

        const QByteArray rfc = fc::util::MimeBuilder::build(m);
        QVERIFY(rfc.contains("In-Reply-To: <orig@y.test>"));
        QVERIFY(rfc.contains("References: <grand@z.test> <orig@y.test>"));
    }
};

QTEST_APPLESS_MAIN(TestMimeBuilder)
#include "test_mime_builder.moc"
