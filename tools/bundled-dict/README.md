# Bundled email-body zstd dictionary

This directory builds the shared compression dictionary that ships
inside FirstContact at `:/compression/bundled-bodies-v1.zdict`.
`MessageRepository::dictionaryFor()` falls back to this resource when
an account has not (yet) trained its own dict.

The pipeline is reproducible from public sources. The dictionary
itself is a binary statistical artifact, not a derivative work of
the corpus — it contains entropy tables plus a content tail of
common patterns. Build it once and check the output into
`resources/compression/`.

## What this directory contains

| File | Purpose |
|---|---|
| `fetch_corpus.sh`        | Downloads public email corpora into `corpus-cache/`. |
| `fetch_newsletters.py`   | Opt-in: fetches modern newsletter HTML via public RSS feeds (politeness rules built in). |
| `newsletter-feeds.txt`   | List of feed URLs the newsletter fetcher uses. |
| `extract_samples.py`     | Walks `corpus-cache/`, decodes MIME, writes per-part sample files to `samples/text/` and `samples/html/`. Parallelized. |
| `seed_content.txt`       | Hand-curated ~10 KiB of high-value boilerplate fed to `ZDICT_finalizeDictionary` as `dictContent`. Sections are marked with `## SectionName`; the trainer strips those lines. |
| `seed_coverage.py`       | Audits per-pattern hit rate in the extracted samples — find dead-weight slots in the seed. |
| `train_bundled_dict.cpp` | C++ tool linked against libzstd; calls `ZDICT_finalizeDictionary` and writes the output dict. |
| `CMakeLists.txt`         | Builds `train_bundled_dict`. Gated by `-DBUILD_BUNDLED_DICT_TRAINER=ON`. |

## Output artifact

```
resources/compression/bundled-bodies-v1.zdict
```

- `dictID`: `0x46430001` (chosen inside the safe range
  `[32768, 2^31-1]` so it cannot collide with a future zstd
  public-registry assignment; ASCII "FC" + version `0001`).
- Target size: 64 KiB (sweep at 16/32/64/110 KiB during training;
  smallest size within 5% of best ratio wins).
- Compression level: `3` (matches `BodyCodec::compress`'s default
  so entropy stats are tuned to the level we actually use).

## Using the dictionary from the standard zstd CLI

The output is a format-compatible zstd dictionary — the stock
`zstd` CLI can use it directly with `-D`. Verified against zstd
v1.5.5; any zstd 1.4+ release works.

```
# Compress a file using the bundled dictionary (level 3 to match
# the trainer's tuning):
zstd -3 -D resources/compression/bundled-bodies-v1.zdict input.eml

# Decompress, supplying the same dictionary:
zstd -d -D resources/compression/bundled-bodies-v1.zdict input.eml.zst

# Inspect a frame's embedded dictID (should print 1178796033 = 0x46430001):
zstd -lv input.eml.zst
```

The dict's first 8 bytes carry the zstd dictionary magic
(`0xEC30A437`) followed by the dictID (`0x46430001`, little-endian).
A frame compressed with this dict records the dictID in its header,
so decompressors can verify the right dictionary was supplied via
`ZSTD_getDictID_fromFrame()`.

## Corpus sources

Three buckets, mixed to ~70% HTML / ~30% plaintext sample bytes
(modern email is HTML-dominant; the dict pays off most on chrome).

### Plaintext (~30%)

- **Enron Email Dataset** — `https://www.cs.cmu.edu/~enron/`
  Public domain (released by FERC during the Enron investigation,
  redistributed by CMU). Covers business replies, signatures,
  quoted-thread structure. Older and ASCII-heavy; capped at ~20% of
  total sample bytes so it doesn't drown out modern patterns.

- **lore.kernel.org** (manual supplement, not auto-fetched).
  lore's git smart-HTTP endpoints reject plain curl (403); the only
  supported bulk-fetch is via `git clone --mirror`, which is
  heavyweight. If you want LKML coverage, follow the recipe in
  `corpus-cache/manual-supplement/README.txt` and drop extracted
  `.mbox` / `.eml` files under that tree.

### HTML (~70%)

- **SpamAssassin public corpus** —
  `https://spamassassin.apache.org/old/publiccorpus/`
  Apache Software Foundation, distributed for spam-filter research.
  Contains both ham and spam; the HTML chrome (table layouts,
  inline-style declarations, ESP wrappers) is the same regardless
  of message category, so we use all of it.

- **Nazario Phishing Corpus** — `https://monkey.org/~jose/phishing/`
  CC-BY-4.0. Phishing emails are deliberately crafted to *imitate*
  real transactional templates — fake PayPal receipts, fake shipping
  notifications, fake bank security alerts. Threat actors copy the
  real chrome (ESP class names, HTML preamble, button styles, footer
  disclaimers) almost verbatim, leaving an inadvertent archive of
  modern transactional-template HTML. Spans 2005–2018+;
  `private-phishing4.mbox` carries the most modern samples and is
  the main reason this source pulls its weight against the older
  Enron / SpamAssassin material. We use it strictly as training
  *input* — the dict is statistical, not republished content.

- **MimeKit / Thunderbird / K-9 test fixtures** — open-source
  realistic samples checked into mail-client test suites. Curated
  to match what real clients actually receive.

### Boilerplate fragments (via `seed_content.txt`, not via fetched samples)

ESP template galleries (Mailchimp, SendGrid, Customer.io,
Klaviyo), USPS Informed Delivery format docs, and major-sender
template structure are folded directly into `seed_content.txt`
rather than crawled. These are factual structural patterns, not
copyrightable creative expression.

## Attribution

The Nazario Phishing Corpus is distributed under CC-BY-4.0, which
requires attribution. The corpus contributes structural chrome
patterns to the trained dictionary; the dictionary itself is
statistical (entropy tables + a content tail) and does not
republish any individual message.

> Nazario Phishing Corpus — J. Nazario,
> https://monkey.org/~jose/phishing/, CC-BY-4.0.

The full upstream LICENSE.txt is fetched alongside the corpus into
`corpus-cache/nazario/LICENSE.txt`.

## Workflow

```
# One-time fetch (~600 MB into corpus-cache/, .gitignored).
./fetch_corpus.sh

# Decode MIME, write sample files (~5-10 minutes).
python3 extract_samples.py

# Build + run trainer. Outputs the .zdict and prints sweep results.
cmake -S ../.. -B ../../build-trainer -DBUILD_BUNDLED_DICT_TRAINER=ON
cmake --build ../../build-trainer --target train_bundled_dict
../../build-trainer/tools/bundled-dict/train_bundled_dict \
    --samples-dir samples \
    --seed seed_content.txt \
    --out ../../resources/compression/bundled-bodies-v1.zdict \
    --size 65536 \
    --dict-id 0x46430001 \
    --level 3 \
    --sweep
```

Or, in one shot from the repo root:

```
make bundled-dict
```

The Makefile target wraps fetch + extract + build + train, then
prints a summary of dict size, sample counts, and held-out
compression ratios.

## What gets checked in vs. what stays local

| Path | In repo? |
|---|---|
| `tools/bundled-dict/*.{sh,py,cpp,txt,md}`         | Yes — recipe is reproducible. |
| `tools/bundled-dict/CMakeLists.txt`               | Yes. |
| `tools/bundled-dict/corpus-cache/`                | **No** — `.gitignored`, large. |
| `tools/bundled-dict/samples/`                     | **No** — `.gitignored`, regenerable. |
| `resources/compression/bundled-bodies-v1.zdict`   | Yes — the shipped artifact. |

## Updating the dictionary

Re-run `make bundled-dict`. The bumped `dictID` (`bundled-bodies-v2`)
goes into a new resource path; old installs that have already
re-compressed against v1 will keep using v1 until they retrain
per-account. The bundled dict ID and the resource filename should
move together so a future loader can pick the correct one.

## Forward-looking notes

Two follow-on improvements live in `docs/backburner.md`:

1. **Seed per-account training with this dict's content tail.**
   Switch `BodyCompressionWorker` from `ZDICT_trainFromBuffer` to
   `ZDICT_finalizeDictionary`, passing this dict's content tail as
   `dictContent`. Per-account dicts become a strict superset.
2. **Hybrid per-frame `dictID` routing.** Maintain a pool of dicts
   and route per-row via `ZSTD_getDictID_fromFrame`. Avoids the
   full-cache rewrite when per-account training finishes. Pinned
   as an idea; complexity not currently justified.
