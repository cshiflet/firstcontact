#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

namespace fc::util {

// zstd-with-trained-dictionary codec for message bodies.
//
// Compressed bytes carry a 1-byte magic prefix so decompress() can
// auto-route between plaintext and compressed payloads — needed during
// the lazy backfill where some rows are still uncompressed under the
// same body_text / body_html columns.
//
// All entry points are stateless; the dictionary is passed in as a
// QByteArray so callers can cache it per-account however they want
// (typically loaded once from account_meta + held in MessageRepository).
class BodyCodec {
public:
    // Magic prefix bytes for compressed payloads. Chosen so they don't
    // collide with legitimate UTF-8 start bytes (0x00-0x7F single-byte
    // ASCII, 0xC2-0xF4 multi-byte leading), and so a stray plaintext
    // body starting with these bytes won't be misread as compressed.
    // \x1F is ASCII US (Unit Separator), \x9D is a non-displayable C1
    // control byte — neither appears in well-formed UTF-8 text.
    static constexpr char kMagicByte0 = '\x1F';
    static constexpr char kMagicByte1 = '\x9D';
    static constexpr int  kHeaderLen  = 2;

    // Train a dictionary from a sample of bodies. Returns the
    // serialized dictionary as a QByteArray on success, or an empty
    // QByteArray on failure (insufficient samples, training error,
    // etc.). `dictSize` is the target output size — zstd typically
    // recommends 64-110 KiB; defaults to 64 KiB.
    //
    // Memory profile: zstd's training algorithm holds all samples
    // contiguously plus working buffers. Callers should cap the
    // sample count (1000-2000 rows is plenty) and the per-sample
    // size (32 KiB cap is reasonable for email bodies) before
    // calling this.
    static QByteArray trainDictionary(const std::vector<QByteArray>& samples,
                                       int dictSize = 64 * 1024);

    // Compress with the given dictionary. Returns kHeaderLen-prefixed
    // payload on success, or an unchanged copy of `plain` on failure
    // (codec error, empty dict). Empty input returns empty output.
    static QByteArray compress(const QByteArray& plain,
                                const QByteArray& dict,
                                int compressionLevel = 3);

    // Decompress with the given dictionary. Auto-detects via the
    // magic prefix: if the input starts with the prefix, treats the
    // rest as zstd payload; otherwise returns the input verbatim
    // (already-plaintext row). Returns an empty QByteArray on codec
    // error (corrupt payload, missing dictionary).
    static QByteArray decompress(const QByteArray& data,
                                  const QByteArray& dict);

    // True if `data` carries the magic prefix. Cheap O(1) check that
    // callers can use to avoid touching the codec for plaintext rows.
    static bool isCompressed(const QByteArray& data);
};

}  // namespace fc::util
