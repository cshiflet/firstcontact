#include "BodyCodec.h"

#include <zdict.h>
#include <zstd.h>

#include <QtGlobal>

#include <algorithm>
#include <vector>

namespace fc::util {

QByteArray BodyCodec::trainDictionary(const std::vector<QByteArray>& samples,
                                       int dictSize) {
    if (samples.empty() || dictSize <= 0) return {};

    // ZDICT_trainFromBuffer wants:
    //   - one contiguous buffer concatenating every sample, no separator
    //   - a parallel size_t[] of per-sample sizes
    // It allocates internal working memory roughly proportional to
    // total sample bytes; cap the sample list at the call site (typical
    // budget: 1000-2000 samples × ≤32 KiB each).
    qsizetype totalBytes = 0;
    for (const auto& s : samples) totalBytes += s.size();
    if (totalBytes <= 0) return {};

    std::vector<char>   buffer(static_cast<size_t>(totalBytes));
    std::vector<size_t> sizes;
    sizes.reserve(samples.size());

    qsizetype offset = 0;
    for (const auto& s : samples) {
        if (s.isEmpty()) continue;
        std::copy(s.constData(), s.constData() + s.size(),
                  buffer.data() + offset);
        offset += s.size();
        sizes.push_back(static_cast<size_t>(s.size()));
    }
    if (sizes.empty()) return {};

    QByteArray dict;
    dict.resize(dictSize);
    const size_t written = ZDICT_trainFromBuffer(
        dict.data(), static_cast<size_t>(dictSize),
        buffer.data(),
        sizes.data(), static_cast<unsigned>(sizes.size()));
    if (ZDICT_isError(written)) {
        qWarning("BodyCodec::trainDictionary failed: %s",
                 ZDICT_getErrorName(written));
        return {};
    }
    dict.resize(static_cast<int>(written));
    return dict;
}

QByteArray BodyCodec::compress(const QByteArray& plain,
                                const QByteArray& dict,
                                int compressionLevel) {
    if (plain.isEmpty()) return {};
    if (dict.isEmpty()) return plain;   // no dict yet — pass through

    // ZSTD_CCtx + ZSTD_CDict pair gives us dictionary-scoped compression
    // without a hot-path init cost. Both stay scoped to this call;
    // creating them per-call is fine at our compression volume (each
    // upsertMany batch is at most ~50 messages, and dict loading is
    // microseconds for a 64 KiB dictionary).
    ZSTD_CCtx*  cctx = ZSTD_createCCtx();
    ZSTD_CDict* cdict = ZSTD_createCDict(dict.constData(),
                                          static_cast<size_t>(dict.size()),
                                          compressionLevel);
    if (!cctx || !cdict) {
        if (cctx) ZSTD_freeCCtx(cctx);
        if (cdict) ZSTD_freeCDict(cdict);
        qWarning("BodyCodec::compress: failed to allocate ZSTD context");
        return plain;
    }

    const size_t bound = ZSTD_compressBound(static_cast<size_t>(plain.size()));
    QByteArray out;
    out.resize(static_cast<int>(bound) + kHeaderLen);
    out[0] = kMagicByte0;
    out[1] = kMagicByte1;

    const size_t written = ZSTD_compress_usingCDict(
        cctx,
        out.data() + kHeaderLen, bound,
        plain.constData(), static_cast<size_t>(plain.size()),
        cdict);

    ZSTD_freeCCtx(cctx);
    ZSTD_freeCDict(cdict);

    if (ZSTD_isError(written)) {
        qWarning("BodyCodec::compress failed: %s", ZSTD_getErrorName(written));
        return plain;
    }
    out.resize(static_cast<int>(written) + kHeaderLen);
    return out;
}

QByteArray BodyCodec::decompress(const QByteArray& data,
                                  const QByteArray& dict) {
    if (data.isEmpty()) return {};
    if (!isCompressed(data)) return data;   // already plaintext
    if (dict.isEmpty()) {
        qWarning("BodyCodec::decompress: compressed payload but no dictionary");
        return {};
    }

    const char* payload = data.constData() + kHeaderLen;
    const size_t payloadSize = static_cast<size_t>(data.size()) - kHeaderLen;

    // Frame can report its decompressed size; fall back to a generous
    // initial buffer if it's unknown / streaming.
    const unsigned long long expected =
        ZSTD_getFrameContentSize(payload, payloadSize);
    size_t outSize = (expected == ZSTD_CONTENTSIZE_ERROR
                       || expected == ZSTD_CONTENTSIZE_UNKNOWN
                       || expected == 0)
        ? std::max<size_t>(payloadSize * 4, 4096)
        : static_cast<size_t>(expected);

    ZSTD_DCtx*  dctx  = ZSTD_createDCtx();
    ZSTD_DDict* ddict = ZSTD_createDDict(dict.constData(),
                                          static_cast<size_t>(dict.size()));
    if (!dctx || !ddict) {
        if (dctx) ZSTD_freeDCtx(dctx);
        if (ddict) ZSTD_freeDDict(ddict);
        qWarning("BodyCodec::decompress: failed to allocate ZSTD context");
        return {};
    }

    QByteArray out;
    out.resize(static_cast<int>(outSize));
    const size_t written = ZSTD_decompress_usingDDict(
        dctx,
        out.data(), outSize,
        payload, payloadSize,
        ddict);

    ZSTD_freeDCtx(dctx);
    ZSTD_freeDDict(ddict);

    if (ZSTD_isError(written)) {
        qWarning("BodyCodec::decompress failed: %s", ZSTD_getErrorName(written));
        return {};
    }
    out.resize(static_cast<int>(written));
    return out;
}

bool BodyCodec::isCompressed(const QByteArray& data) {
    return data.size() >= kHeaderLen
        && data[0] == kMagicByte0
        && data[1] == kMagicByte1;
}

}  // namespace fc::util
