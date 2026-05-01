#include "util/Base64Url.h"

#include <QObject>
#include <QtTest>

class TestBase64Url : public QObject {
    Q_OBJECT
private slots:
    void roundTripBytes_data() {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("empty")  << QByteArray("");
        QTest::newRow("simple") << QByteArray("hello");
        QTest::newRow("binary") << QByteArray::fromHex("ff00aabbccddeeff");
        QTest::newRow("rfc")    << QByteArray("light w");  // pads in std b64
    }

    void roundTripBytes() {
        QFETCH(QByteArray, input);
        const QByteArray enc = fc::util::base64UrlEncode(input);
        QVERIFY(!enc.contains('+'));
        QVERIFY(!enc.contains('/'));
        QVERIFY(!enc.contains('='));
        QCOMPARE(fc::util::base64UrlDecode(enc), input);
    }
};

QTEST_APPLESS_MAIN(TestBase64Url)
#include "test_base64url.moc"
