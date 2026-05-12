#!/usr/bin/env bash
# Fetches public email corpora used to train the bundled body-compression
# dictionary. All sources are public; the trained dict is a statistical
# artifact, not a derivative of any specific message.
#
# Output: ./corpus-cache/<source>/... (gitignored)
#
# Re-running is idempotent: each source's tarball is downloaded once and
# extracted under its own directory. Delete corpus-cache/ to start fresh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE_DIR="${SCRIPT_DIR}/corpus-cache"
mkdir -p "${CACHE_DIR}"

# Resilient fetch: don't abort the whole script if one mirror is down —
# we want a partial corpus rather than nothing. Report which sources
# succeeded at the end.
declare -a OK_SOURCES=()
declare -a FAILED_SOURCES=()

fetch_url() {
    local name="$1"
    local url="$2"
    local out="$3"
    if [[ -s "${out}" ]]; then
        echo "→ ${name}: already cached (${out})"
        return 0
    fi
    echo "→ ${name}: fetching ${url}"
    if curl -fsSL --retry 3 --retry-delay 5 -o "${out}.tmp" "${url}"; then
        mv "${out}.tmp" "${out}"
        return 0
    else
        rm -f "${out}.tmp"
        return 1
    fi
}

extract_to() {
    local archive="$1"
    local dest="$2"
    mkdir -p "${dest}"
    case "${archive}" in
        *.tar.gz|*.tgz)  tar -xzf "${archive}" -C "${dest}" ;;
        *.tar.bz2|*.tbz2) tar -xjf "${archive}" -C "${dest}" ;;
        *.tar.xz)        tar -xJf "${archive}" -C "${dest}" ;;
        *.gz)            gunzip -kc "${archive}" > "${dest}/$(basename "${archive}" .gz)" ;;
        *.zip)           unzip -qo "${archive}" -d "${dest}" ;;
        *) echo "  ! unknown archive format: ${archive}"; return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# 1. SpamAssassin public corpus — Apache, distributed for spam-filter
#    research. Mixed ham/spam, the only realistic public source of modern
#    HTML newsletter chrome.
# ---------------------------------------------------------------------------
SA_BASE="https://spamassassin.apache.org/old/publiccorpus"
SA_FILES=(
    20021010_easy_ham.tar.bz2
    20021010_hard_ham.tar.bz2
    20021010_spam.tar.bz2
    20030228_easy_ham.tar.bz2
    20030228_easy_ham_2.tar.bz2
    20030228_hard_ham.tar.bz2
    20030228_spam.tar.bz2
    20030228_spam_2.tar.bz2
    20050311_spam_2.tar.bz2
)
SA_DIR="${CACHE_DIR}/spamassassin"
mkdir -p "${SA_DIR}/archives"
sa_any_ok=0
for f in "${SA_FILES[@]}"; do
    if fetch_url "spamassassin/${f}" "${SA_BASE}/${f}" \
                  "${SA_DIR}/archives/${f}"; then
        extract_to "${SA_DIR}/archives/${f}" "${SA_DIR}/extracted" || true
        sa_any_ok=1
    fi
done
if (( sa_any_ok )); then
    OK_SOURCES+=("spamassassin")
else
    FAILED_SOURCES+=("spamassassin")
fi

# ---------------------------------------------------------------------------
# 2. Enron — CMU public-domain mirror. ~423 MB. Plaintext only.
# ---------------------------------------------------------------------------
ENRON_URL="https://www.cs.cmu.edu/~enron/enron_mail_20150507.tar.gz"
ENRON_DIR="${CACHE_DIR}/enron"
mkdir -p "${ENRON_DIR}"
if fetch_url "enron" "${ENRON_URL}" "${ENRON_DIR}/enron_mail.tar.gz"; then
    if [[ ! -d "${ENRON_DIR}/extracted" ]]; then
        extract_to "${ENRON_DIR}/enron_mail.tar.gz" "${ENRON_DIR}/extracted"
    fi
    OK_SOURCES+=("enron")
else
    echo "  (Enron mirror may be intermittent; not fatal.)"
    FAILED_SOURCES+=("enron")
fi

# ---------------------------------------------------------------------------
# 3. lore.kernel.org — NOT auto-fetched.
#    lore restricts plain curl access to its git smart-HTTP endpoints
#    (returns 403 on /info/refs and /objects/info/packs paths). The
#    only supported bulk-fetch is via public-inbox git clones:
#
#        git clone --mirror https://lore.kernel.org/lkml/0 lkml-0
#        # then extract messages with public-inbox-extindex or similar
#
#    That's hundreds of MB per epoch and slow; not worth automating
#    for our pattern-variety needs. Documented here as a manual
#    supplement option — drop any .mbox / .mbox.gz files under
#    corpus-cache/manual-supplement/lore-lkml/ to include them.
# ---------------------------------------------------------------------------
mkdir -p "${CACHE_DIR}/manual-supplement/lore-lkml"

# ---------------------------------------------------------------------------
# 4. Nazario Phishing Corpus — public mirror at monkey.org/~jose/phishing/,
#    CC-BY-4.0 licensed (see README in corpus-cache/nazario/ after fetch).
#
#    Why phishing for a *compression* dict: phishing emails are
#    deliberately crafted to imitate real transactional templates —
#    fake PayPal receipts, fake bank security alerts, fake shipping
#    notifications. The threat actors copy the *chrome* (ESP class
#    names, HTML preamble, footer disclaimers, button styles) almost
#    verbatim. The result is an inadvertent corpus of MODERN
#    transactional-template HTML, which is exactly the gap our
#    Enron + SpamAssassin (2001-2005) sources can't fill.
#
#    Spans 2005 through 2018+, with private-phishing4.mbox carrying
#    the most modern HTML chrome. We use it strictly as training
#    INPUT — the trained dict is statistical (entropy tables +
#    structural patterns), not republished content.
#
#    Attribution: "Nazario Phishing Corpus, J. Nazario, monkey.org/
#    ~jose/phishing/, CC-BY-4.0" — see tools/bundled-dict/README.md.
# ---------------------------------------------------------------------------
NAZARIO_BASE="https://monkey.org/~jose/phishing"
NAZARIO_FILES=(
    phishing0.mbox
    phishing1.mbox
    phishing2.mbox
    phishing3.mbox
    20051114.mbox
    private-phishing4.mbox
)
NAZARIO_DIR="${CACHE_DIR}/nazario"
mkdir -p "${NAZARIO_DIR}/mboxes"
nazario_any_ok=0
for f in "${NAZARIO_FILES[@]}"; do
    if fetch_url "nazario/${f}" "${NAZARIO_BASE}/${f}" \
                  "${NAZARIO_DIR}/mboxes/${f}"; then
        nazario_any_ok=1
    fi
done
# Pull the LICENSE so the attribution requirement is satisfiable
# offline. README is optional — failure here doesn't block training.
fetch_url "nazario/LICENSE.txt" "${NAZARIO_BASE}/LICENSE.txt" \
          "${NAZARIO_DIR}/LICENSE.txt" || true
if (( nazario_any_ok )); then
    OK_SOURCES+=("nazario")
else
    FAILED_SOURCES+=("nazario")
fi

# ---------------------------------------------------------------------------
# 5. MimeKit test fixtures — realistic mail samples from the canonical
#    .NET MIME library, MIT-licensed. Great coverage of edge-case
#    encodings (quoted-printable splits, base64, multipart/alternative).
# ---------------------------------------------------------------------------
MIMEKIT_DIR="${CACHE_DIR}/mimekit"
mkdir -p "${MIMEKIT_DIR}"
if fetch_url "mimekit fixtures" \
             "https://github.com/jstedfast/MimeKit/archive/refs/heads/master.tar.gz" \
             "${MIMEKIT_DIR}/master.tar.gz"; then
    if [[ ! -d "${MIMEKIT_DIR}/extracted" ]]; then
        extract_to "${MIMEKIT_DIR}/master.tar.gz" "${MIMEKIT_DIR}/extracted"
    fi
    OK_SOURCES+=("mimekit")
else
    FAILED_SOURCES+=("mimekit")
fi

# ---------------------------------------------------------------------------
# 6. Manual-supplement directory. Anything the user drops here is picked
#    up by extract_samples.py. Documented for transparency:
#      - ESP template gallery exports (Mailchimp, SendGrid, etc.)
#      - Snapshots of one's own non-PII subscriptions saved as .eml
#      - Vendor docs that publish their email templates (USPS, GitHub)
#    None of these are auto-fetched because they require manual selection
#    or sign-in. Empty by default.
# ---------------------------------------------------------------------------
mkdir -p "${CACHE_DIR}/manual-supplement"
cat > "${CACHE_DIR}/manual-supplement/README.txt" <<'EOF'
Drop additional .eml / .mbox / .mbox.gz files here. The extractor
walks this directory recursively, same as the auto-fetched sources.

Sensible additions:
  - mail-client test fixtures (Thunderbird comm-central, K-9 mail tests)
  - ESP template galleries exported as .eml
  - Public archive dumps not covered by fetch_corpus.sh
  - lore.kernel.org public-inbox clones (drop in ./lore-lkml/):
        git clone --mirror https://lore.kernel.org/lkml/0 lkml-0
        # extract messages to .eml files using public-inbox tools

Do NOT drop your personal mailbox here — the bundled dict ships with
every install, and the dict's content tail can preserve recognizable
short substrings. Personal mailbox training is what the per-account
worker (BodyCompressionWorker) is for.
EOF

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "=== fetch_corpus.sh summary ==="
echo "Cache root: ${CACHE_DIR}"
echo "OK:     ${OK_SOURCES[*]:-(none)}"
echo "Failed: ${FAILED_SOURCES[*]:-(none)}"
echo
echo "Total cache size:"
du -sh "${CACHE_DIR}" 2>/dev/null || true

if (( ${#OK_SOURCES[@]} == 0 )); then
    echo
    echo "! No sources fetched. The extractor will produce no samples."
    exit 1
fi
