// train_bundled_dict — build the shared zstd dictionary that ships at
// resources/compression/bundled-bodies-v1.zdict.
//
// Pipeline:
//   1. Read seed content (becomes the dict's content tail).
//   2. Walk samples/ recursively, load each file into memory.
//   3. Split into train / held-out (last 10% for ratio measurement).
//   4. Call ZDICT_finalizeDictionary with a fixed dictID.
//   5. Self-test: compress + decompress every held-out sample with the
//      resulting dict, report total ratio and round-trip integrity.
//   6. Optionally sweep {16, 32, 64, 110} KiB and report each size.
//
// Standalone CLI — no Qt, no firstcontact code. Built only when
// -DBUILD_BUNDLED_DICT_TRAINER=ON.

#include <zdict.h>
#include <zstd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr unsigned kDefaultDictId      = 0x46430001u;   // "FC" + v1
constexpr int      kDefaultDictSize    = 64 * 1024;
constexpr int      kDefaultLevel       = 3;
constexpr double   kHeldOutFraction    = 0.10;
constexpr size_t   kHeldOutMinSamples  = 50;

struct Args {
    std::string samplesDir;
    std::string seedPath;
    std::string outPath;
    int         dictSize = kDefaultDictSize;
    unsigned    dictId   = kDefaultDictId;
    int         level    = kDefaultLevel;
    bool        sweep    = false;
    bool        verbose  = false;
};

void printUsage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s --samples-dir <dir> --seed <file> --out <file>\n"
        "          [--size <bytes>=65536]\n"
        "          [--dict-id <int>=0x46430001]\n"
        "          [--level <int>=3]\n"
        "          [--sweep]   train at 16/32/64/110 KiB and report\n"
        "          [--verbose]\n",
        argv0);
}

bool parseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if      (k == "--samples-dir") { auto v = need("--samples-dir"); if (!v) return false; a.samplesDir = v; }
        else if (k == "--seed")        { auto v = need("--seed");        if (!v) return false; a.seedPath = v; }
        else if (k == "--out")         { auto v = need("--out");         if (!v) return false; a.outPath = v; }
        else if (k == "--size")        { auto v = need("--size");        if (!v) return false; a.dictSize = std::atoi(v); }
        else if (k == "--dict-id")     { auto v = need("--dict-id");     if (!v) return false; a.dictId = static_cast<unsigned>(std::strtoul(v, nullptr, 0)); }
        else if (k == "--level")       { auto v = need("--level");       if (!v) return false; a.level = std::atoi(v); }
        else if (k == "--sweep")       { a.sweep = true; }
        else if (k == "--verbose" || k == "-v") { a.verbose = true; }
        else if (k == "-h" || k == "--help") { printUsage(argv[0]); return false; }
        else { std::fprintf(stderr, "unknown arg: %s\n", argv[i]); printUsage(argv[0]); return false; }
    }
    if (a.samplesDir.empty() || a.seedPath.empty() || a.outPath.empty()) {
        printUsage(argv[0]);
        return false;
    }
    return true;
}

std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    if (sz <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<char> out(static_cast<size_t>(sz));
    f.read(out.data(), sz);
    if (!f) return {};
    return out;
}

// Strip lines that begin with "## " from seed content. Those are
// section markers used by seed_coverage.py and by humans; they would
// otherwise occupy slots in the dict content tail without ever
// matching real email. ~30 lines * ~30 bytes ≈ 1 KiB recovered out of
// a 64 KiB dict.
std::vector<char> stripSectionMarkers(const std::vector<char>& src) {
    std::vector<char> out;
    out.reserve(src.size());
    size_t i = 0;
    while (i < src.size()) {
        // Find end of this line.
        size_t j = i;
        while (j < src.size() && src[j] != '\n') ++j;
        const bool isMarker = (j - i) >= 3
                            && src[i] == '#' && src[i+1] == '#'
                            && src[i+2] == ' ';
        if (!isMarker) {
            // Copy this line (including the trailing \n if present).
            out.insert(out.end(),
                       src.begin() + static_cast<std::ptrdiff_t>(i),
                       src.begin() + static_cast<std::ptrdiff_t>(
                           std::min(j + 1, src.size())));
        }
        i = j + 1;
    }
    return out;
}

bool writeFile(const std::string& path, const void* data, size_t size) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

// One training sample: the raw bytes plus a flag for which subtree it
// came from. Keeping the kind paired with the data lets us split the
// held-out test by kind so compression ratio is reportable across
// runs with different corpus composition (a pure-text test inflates
// the average ratio because plaintext compresses worse).
struct Sample {
    std::vector<char> data;
    bool              isHtml = false;
};

// Walk samples-dir recursively, returning every regular file's path.
// Samples are split text/ vs html/ subtrees; we treat them
// equivalently for training (one dict covers both) but report counts
// per kind so the operator can see what landed.
struct LoadedSamples {
    std::vector<Sample> samples;
    size_t totalBytes = 0;
    size_t textCount  = 0;
    size_t htmlCount  = 0;
    size_t textBytes  = 0;
    size_t htmlBytes  = 0;
};

LoadedSamples loadSamples(const std::string& root, bool verbose) {
    LoadedSamples r;
    if (!fs::is_directory(root)) {
        std::fprintf(stderr, "samples-dir not a directory: %s\n", root.c_str());
        return r;
    }
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        const auto p = e.path();
        const auto name = p.filename().string();
        // Skip the manifest index emitted by extract_samples.py.
        if (name == "index.json") continue;
        auto blob = readFile(p.string());
        if (blob.size() < 8) continue;   // zstd's minimum sample size

        // Categorize by whichever subtree the file is under. "samples/
        // text/..." → text, "samples/html/..." → html, anything else
        // gets counted as text by default.
        const std::string ps = p.string();
        const bool isHtml = ps.find("/html/") != std::string::npos
                         || ps.find("\\html\\") != std::string::npos;
        if (isHtml) { ++r.htmlCount; r.htmlBytes += blob.size(); }
        else        { ++r.textCount; r.textBytes += blob.size(); }
        r.totalBytes += blob.size();
        r.samples.push_back(Sample{std::move(blob), isHtml});
    }
    if (verbose) {
        std::fprintf(stderr,
            "  loaded %zu samples (%zu text / %zu html), %.2f MiB total\n",
            r.samples.size(), r.textCount, r.htmlCount,
            r.totalBytes / (1024.0 * 1024.0));
    }
    return r;
}

// Build the parallel buffer + sizes array that ZDICT_finalizeDictionary
// expects. The input is moved-from to avoid double-allocation.
struct FlatSamples {
    std::vector<char>   buffer;
    std::vector<size_t> sizes;
};

FlatSamples flatten(const std::vector<Sample>& samples,
                    size_t begin, size_t end) {
    FlatSamples f;
    size_t total = 0;
    for (size_t i = begin; i < end; ++i) total += samples[i].data.size();
    f.buffer.resize(total);
    f.sizes.reserve(end - begin);
    size_t off = 0;
    for (size_t i = begin; i < end; ++i) {
        const auto& b = samples[i].data;
        std::memcpy(f.buffer.data() + off, b.data(), b.size());
        off += b.size();
        f.sizes.push_back(b.size());
    }
    return f;
}

// Per-kind held-out statistics. Reported separately so different
// corpus compositions (more vs less HTML) produce apples-to-apples
// numbers across runs.
struct KindStats {
    size_t samples = 0;
    size_t plainBytes = 0;
    size_t compressedBytes = 0;
    double ratio() const {
        return plainBytes
             ? static_cast<double>(compressedBytes)
             / static_cast<double>(plainBytes)
             : 0.0;
    }
};

struct TrainResult {
    std::vector<char> dictBytes;
    int               targetSize;
    KindStats         all;
    KindStats         text;
    KindStats         html;
    bool              roundTripOk = true;
};

TrainResult train(const Args& a,
                  const std::vector<char>& seed,
                  const std::vector<Sample>& samples,
                  size_t trainEnd,
                  size_t heldOutBegin,
                  int targetSize) {
    TrainResult r;
    r.targetSize = targetSize;

    auto flat = flatten(samples, 0, trainEnd);

    // ZDICT_finalizeDictionary treats `dictContent` as the entirety of
    // the dict's content tail — it does NOT pull additional content
    // from `samplesBuffer` to fill maxDictSize. With a 10 KiB seed and
    // a 64 KiB target, that leaves ~54 KiB of dict slots unfilled.
    //
    // To use the full budget, bootstrap with ZDICT_trainFromBuffer
    // first to extract a content tail from the samples (sized to fit
    // the remainder budget), strip its zstd header via
    // ZDICT_getDictHeaderSize, then concat [extracted_tail || seed]
    // and pass that combined buffer as dictContent. The seed stays at
    // the end of the tail where zstd places highest-back-reference
    // value.
    //
    // Header overhead is ~150 bytes in practice; leave a generous
    // 512-byte buffer so finalizeDictionary doesn't have to truncate
    // the front of the extracted content.
    constexpr size_t kHeaderReserve = 512;
    const size_t totalContentBudget = static_cast<size_t>(targetSize)
                                    - std::min<size_t>(kHeaderReserve,
                                                       targetSize / 4);

    std::vector<char> extracted;
    if (seed.size() + 1024 < totalContentBudget) {
        const size_t extractBudget = totalContentBudget - seed.size();
        // trainFromBuffer's output is `[header][content]`; oversize the
        // buffer slightly so we get the full requested content tail.
        std::vector<char> trainedDict(extractBudget + kHeaderReserve);
        const size_t trainedSize = ZDICT_trainFromBuffer(
            trainedDict.data(), trainedDict.size(),
            flat.buffer.data(),
            flat.sizes.data(),
            static_cast<unsigned>(flat.sizes.size()));
        if (ZDICT_isError(trainedSize)) {
            if (a.verbose) {
                std::fprintf(stderr,
                    "  trainFromBuffer bootstrap failed: %s — "
                    "falling back to seed-only dict\n",
                    ZDICT_getErrorName(trainedSize));
            }
        } else {
            const size_t headerSize = ZDICT_getDictHeaderSize(
                trainedDict.data(), trainedSize);
            if (!ZDICT_isError(headerSize) && headerSize < trainedSize) {
                extracted.assign(
                    trainedDict.begin() + static_cast<std::ptrdiff_t>(headerSize),
                    trainedDict.begin() + static_cast<std::ptrdiff_t>(trainedSize));
                if (a.verbose) {
                    std::fprintf(stderr,
                        "  bootstrap: %zu bytes extracted from samples "
                        "(header %zu stripped)\n",
                        extracted.size(), headerSize);
                }
            }
        }
    }

    // [extracted_patterns || seed] — seed last so it gets the
    // highest-value tail slots.
    std::vector<char> dictContent;
    dictContent.reserve(extracted.size() + seed.size());
    dictContent.insert(dictContent.end(),
                       extracted.begin(), extracted.end());
    dictContent.insert(dictContent.end(), seed.begin(), seed.end());

    r.dictBytes.resize(static_cast<size_t>(targetSize));

    ZDICT_params_t params{};
    params.compressionLevel = a.level;
    params.notificationLevel = a.verbose ? 2 : 0;
    params.dictID = a.dictId;

    const size_t produced = ZDICT_finalizeDictionary(
        r.dictBytes.data(), r.dictBytes.size(),
        dictContent.data(), dictContent.size(),
        flat.buffer.data(),
        flat.sizes.data(),
        static_cast<unsigned>(flat.sizes.size()),
        params);
    if (ZDICT_isError(produced)) {
        std::fprintf(stderr, "ZDICT_finalizeDictionary failed: %s\n",
                     ZDICT_getErrorName(produced));
        r.dictBytes.clear();
        return r;
    }
    r.dictBytes.resize(produced);

    // --- Held-out self-test ---
    ZSTD_CCtx*  cctx  = ZSTD_createCCtx();
    ZSTD_DCtx*  dctx  = ZSTD_createDCtx();
    ZSTD_CDict* cdict = ZSTD_createCDict(r.dictBytes.data(),
                                          r.dictBytes.size(),
                                          a.level);
    ZSTD_DDict* ddict = ZSTD_createDDict(r.dictBytes.data(),
                                          r.dictBytes.size());

    std::vector<char> outBuf;
    std::vector<char> plainBuf;
    for (size_t i = heldOutBegin; i < samples.size(); ++i) {
        const auto& sample = samples[i].data;
        const size_t bound = ZSTD_compressBound(sample.size());
        outBuf.resize(bound);
        const size_t cz = ZSTD_compress_usingCDict(
            cctx, outBuf.data(), outBuf.size(),
            sample.data(), sample.size(), cdict);
        if (ZSTD_isError(cz)) {
            std::fprintf(stderr, "compress held-out %zu: %s\n",
                         i, ZSTD_getErrorName(cz));
            r.roundTripOk = false;
            continue;
        }
        plainBuf.resize(sample.size());
        const size_t dz = ZSTD_decompress_usingDDict(
            dctx, plainBuf.data(), plainBuf.size(),
            outBuf.data(), cz, ddict);
        if (ZSTD_isError(dz) || dz != sample.size() ||
            std::memcmp(plainBuf.data(), sample.data(), dz) != 0) {
            std::fprintf(stderr, "round-trip mismatch on held-out %zu\n", i);
            r.roundTripOk = false;
            continue;
        }
        KindStats& bucket = samples[i].isHtml ? r.html : r.text;
        bucket.samples++;
        bucket.plainBytes      += sample.size();
        bucket.compressedBytes += cz;
        r.all.samples++;
        r.all.plainBytes      += sample.size();
        r.all.compressedBytes += cz;
    }

    ZSTD_freeCCtx(cctx);
    ZSTD_freeDCtx(dctx);
    ZSTD_freeCDict(cdict);
    ZSTD_freeDDict(ddict);
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parseArgs(argc, argv, a)) return 2;

    std::fprintf(stderr, "→ reading seed: %s\n", a.seedPath.c_str());
    auto rawSeed = readFile(a.seedPath);
    if (rawSeed.empty()) {
        std::fprintf(stderr, "seed file empty or unreadable: %s\n",
                     a.seedPath.c_str());
        return 1;
    }
    auto seed = stripSectionMarkers(rawSeed);
    std::fprintf(stderr,
        "  seed size: %zu bytes (raw %zu, %zu stripped from section markers)\n",
        seed.size(), rawSeed.size(), rawSeed.size() - seed.size());

    std::fprintf(stderr, "→ loading samples from: %s\n", a.samplesDir.c_str());
    auto loaded = loadSamples(a.samplesDir, a.verbose);
    if (loaded.samples.size() < 100) {
        std::fprintf(stderr,
            "! only %zu samples found — need at least ~100 for a useful dict\n",
            loaded.samples.size());
        if (loaded.samples.empty()) return 1;
    }

    // Deterministic shuffle so train/held-out split is stable across
    // runs but still mixes the corpus order (sources tend to cluster
    // by directory walk).
    std::mt19937 rng(0x46430001u);
    std::shuffle(loaded.samples.begin(), loaded.samples.end(), rng);

    size_t heldOutCount = std::max(
        kHeldOutMinSamples,
        static_cast<size_t>(loaded.samples.size() * kHeldOutFraction));
    heldOutCount = std::min(heldOutCount, loaded.samples.size() / 2);
    const size_t trainCount = loaded.samples.size() - heldOutCount;
    std::fprintf(stderr,
        "  split: %zu train / %zu held-out\n", trainCount, heldOutCount);

    std::vector<int> sizes;
    if (a.sweep) sizes = {16 * 1024, 32 * 1024, 64 * 1024, 110 * 1024};
    else         sizes = {a.dictSize};

    std::vector<TrainResult> results;
    results.reserve(sizes.size());
    for (int sz : sizes) {
        std::fprintf(stderr, "\n→ training at %d KiB\n", sz / 1024);
        auto r = train(a, seed, loaded.samples, trainCount, trainCount, sz);
        if (r.dictBytes.empty()) {
            std::fprintf(stderr, "training failed at %d KiB\n", sz / 1024);
            return 1;
        }
        std::fprintf(stderr,
            "  produced dict: %zu bytes, round-trip %s\n"
            "    held-out overall: ratio %.4f (%.2f%% of plain) "
            "on %zu samples\n"
            "    held-out text:    ratio %.4f (%.2f%%) on %zu samples\n"
            "    held-out html:    ratio %.4f (%.2f%%) on %zu samples\n",
            r.dictBytes.size(), r.roundTripOk ? "OK" : "FAIL",
            r.all.ratio(),  r.all.ratio()  * 100.0, r.all.samples,
            r.text.ratio(), r.text.ratio() * 100.0, r.text.samples,
            r.html.ratio(), r.html.ratio() * 100.0, r.html.samples);
        results.push_back(std::move(r));
    }

    // Pick the result to ship. In single-size mode that's just
    // results[0]. In sweep mode, pick the smallest size whose ratio
    // is within 5% of the best (smaller dict = smaller binary).
    size_t pick = 0;
    if (results.size() > 1) {
        double bestRatio = results[0].all.ratio();
        for (auto& r : results) bestRatio = std::min(bestRatio, r.all.ratio());
        const double threshold = bestRatio * 1.05;
        for (size_t i = 0; i < results.size(); ++i) {
            if (results[i].roundTripOk
                && results[i].all.ratio() <= threshold) {
                pick = i;
                break;
            }
        }
        std::fprintf(stderr,
            "\n→ sweep choice: %d KiB (ratio %.4f), within 5%% of best %.4f\n",
            results[pick].targetSize / 1024,
            results[pick].all.ratio(), bestRatio);
    }
    if (!results[pick].roundTripOk) {
        std::fprintf(stderr,
            "! chosen dict failed round-trip self-test — refusing to write\n");
        return 1;
    }

    // Sanity-check the dictID embedded in the output. ZDICT will obey
    // our params.dictID, but verifying makes a future "did this dict
    // come out of our pipeline?" check trivial.
    const unsigned actualId = ZDICT_getDictID(
        results[pick].dictBytes.data(), results[pick].dictBytes.size());
    if (actualId != a.dictId) {
        std::fprintf(stderr,
            "! dictID mismatch: requested 0x%08x got 0x%08x\n",
            a.dictId, actualId);
        return 1;
    }

    if (!writeFile(a.outPath, results[pick].dictBytes.data(),
                   results[pick].dictBytes.size())) {
        std::fprintf(stderr, "failed to write output: %s\n", a.outPath.c_str());
        return 1;
    }

    const auto& chosen = results[pick];
    std::fprintf(stderr,
        "\n=== bundled dict written ===\n"
        "Path:           %s\n"
        "Size:           %zu bytes (target %d KiB)\n"
        "dictID:         0x%08x\n"
        "Level:          %d\n"
        "Train rows:     %zu\n"
        "Held-out total: %zu rows, ratio %.4f (%.1f%% of plain)\n"
        "  text:         %zu rows, ratio %.4f (%.1f%%)\n"
        "  html:         %zu rows, ratio %.4f (%.1f%%)\n",
        a.outPath.c_str(),
        chosen.dictBytes.size(),
        chosen.targetSize / 1024,
        actualId, a.level, trainCount,
        chosen.all.samples,  chosen.all.ratio(),  chosen.all.ratio()  * 100.0,
        chosen.text.samples, chosen.text.ratio(), chosen.text.ratio() * 100.0,
        chosen.html.samples, chosen.html.ratio(), chosen.html.ratio() * 100.0);
    return 0;
}
