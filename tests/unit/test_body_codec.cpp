#include "util/BodyCodec.h"

#include <QObject>
#include <QtTest>

#include <vector>

namespace {

// Build a corpus of ~200 short HTML-ish samples that share a lot of
// boilerplate. With a dictionary the codec should get far better ratios
// than plain zstd would on each isolated body.
std::vector<QByteArray> emailLikeCorpus() {
    std::vector<QByteArray> out;
    out.reserve(200);
    for (int i = 0; i < 200; ++i) {
        QByteArray b;
        b += "<!doctype html><html><body>";
        b += "<table cellpadding=0 cellspacing=0 border=0 width=600>";
        b += "<tr><td><a href=\"https://newsletter.example.com/unsub/" +
             QByteArray::number(i) + "\">unsubscribe</a></td></tr>";
        b += "<tr><td>Hi customer #" + QByteArray::number(i) +
             ", your weekly digest is below.</td></tr>";
        b += "<tr><td>Click <a href=\"https://example.com/x/" +
             QByteArray::number(i) + "\">here</a> to read more.</td></tr>";
        b += "</table>";
        b += "<p>This message was sent to you because you signed up. ";
        b += "Sent from a privacy-respecting client.</p>";
        b += "</body></html>";
        out.push_back(std::move(b));
    }
    return out;
}

}  // namespace

class TestBodyCodec : public QObject {
    Q_OBJECT
private slots:
    void roundTripWithDictionary() {
        const auto corpus = emailLikeCorpus();
        const QByteArray dict = fc::util::BodyCodec::trainDictionary(corpus, 32 * 1024);
        QVERIFY2(!dict.isEmpty(), "training should produce a non-empty dictionary");

        // Pick one sample (not in the path the dict was trained on
        // exclusively — full corpus was used; this just verifies round
        // trip integrity).
        const QByteArray plain = corpus.front();
        const QByteArray comp  = fc::util::BodyCodec::compress(plain, dict);
        QVERIFY(fc::util::BodyCodec::isCompressed(comp));
        // 1 KB+ of HTML compresses well with a trained dictionary.
        QVERIFY2(comp.size() < plain.size(),
                 "compressed payload should be smaller than plaintext");
        const QByteArray back  = fc::util::BodyCodec::decompress(comp, dict);
        QCOMPARE(back, plain);
    }

    void passThroughWithoutDictionary() {
        const QByteArray plain = "the quick brown fox";
        const QByteArray out   = fc::util::BodyCodec::compress(plain, QByteArray{});
        QCOMPARE(out, plain);   // no dict yet = identity
        QVERIFY(!fc::util::BodyCodec::isCompressed(out));
    }

    void decompressReturnsPlaintextRowsUntouched() {
        // The lazy-backfill path leaves old rows uncompressed under
        // the same column. decompress() must recognise this and
        // return them as-is rather than failing.
        const QByteArray plain = "<html>legacy uncompressed row</html>";
        const QByteArray dict  = "ignored-because-not-compressed";
        QCOMPARE(fc::util::BodyCodec::decompress(plain, dict), plain);
    }

    void emptyInputProducesEmptyOutput() {
        const QByteArray dict = "any";
        QCOMPARE(fc::util::BodyCodec::compress(QByteArray{}, dict),    QByteArray{});
        QCOMPARE(fc::util::BodyCodec::decompress(QByteArray{}, dict),  QByteArray{});
    }

    void magicBytesDontCollideWithUtf8() {
        // The chosen prefix (\x1F \x9D) is not a valid UTF-8 sequence
        // start, so a plaintext body that happens to begin with the
        // magic bytes would have to be malformed text. Verify
        // isCompressed() catches it though — better-safe-than-sorry.
        QByteArray faux;
        faux.append(fc::util::BodyCodec::kMagicByte0);
        faux.append(fc::util::BodyCodec::kMagicByte1);
        faux.append("garbage");
        QVERIFY(fc::util::BodyCodec::isCompressed(faux));
        // And: any "real" plaintext that doesn't start with the prefix
        // should NOT be flagged.
        QVERIFY(!fc::util::BodyCodec::isCompressed(QByteArray("hello")));
    }
};

QTEST_APPLESS_MAIN(TestBodyCodec)
#include "test_body_codec.moc"
