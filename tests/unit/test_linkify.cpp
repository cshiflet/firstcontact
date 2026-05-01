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
};

QTEST_APPLESS_MAIN(TestLinkify)
#include "test_linkify.moc"
