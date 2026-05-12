#!/usr/bin/env python3
"""
extract_samples.py — Walk corpus-cache/, decode MIME parts, write
per-part sample files into samples/text/ and samples/html/.

Output layout:
    samples/
        text/<source>/<hash>.txt   (text/plain bodies)
        html/<source>/<hash>.html  (text/html bodies)
        index.json                 (sample counts + size totals)

Each sample is the decoded body bytes of one MIME part (post
quoted-printable / base64). Headers are not included — the runtime
cache compresses body columns, not headers.

Per-sample size cap matches BodyCompressionWorker::kTrainingPerSampleCap
(32 KiB) so the bundled dict is trained on samples that look like the
samples per-account training will see.

Target byte balance: ~70% HTML / ~30% plaintext. The walker tracks
cumulative bytes per kind and skips additional samples in whichever
kind is over budget — keeps any one source (e.g., Enron's plaintext
volume) from dominating.

Run from this directory:
    python3 extract_samples.py
or with overrides:
    python3 extract_samples.py --corpus-dir corpus-cache --out samples \\
        --max-samples-per-source 5000
"""

from __future__ import annotations

import argparse
import email
import email.policy
import hashlib
import json
import mailbox
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from typing import Iterable

# Match kTrainingPerSampleCap in BodyCompressionWorker.cpp.
PER_SAMPLE_CAP = 32 * 1024

# Minimum sample size — zstd's training algorithm requires >= 8 bytes
# per sample, and bodies under ~64 bytes are usually just "Thanks!"
# noise that doesn't contribute pattern variety.
MIN_SAMPLE_BYTES = 64

# Target balance (~70% HTML / ~30% plaintext). When either kind goes
# past its byte budget the walker stops adding more of that kind.
#
# Default 128 MiB total — 16x zstd's "samples ≈ 100× dict size"
# recommendation. At this size the bootstrap step is finding
# essentially all the pattern variety the corpus contains; returns
# beyond this are vanishing. Override with --budget-mib at runtime.
# Peak RAM in the trainer is roughly 1.5x the budget (training buffer
# + ZDICT working memory + dict output).
DEFAULT_BUDGET_MIB = 128
HTML_FRACTION = 0.70
TEXT_FRACTION = 0.30


_SKIP_SUFFIXES = {".txt", ".html", ".gz", ".bz2", ".xz", ".zip", ".tar"}


def classify_file(p: Path) -> str | None:
    """Return 'mbox' / 'eml' / None — one stat per file, no peek-read.

    This is the hot path: with 500k+ files under corpus-cache/ (Enron
    alone), splitting the old is_mbox_file + is_eml_file pair (each of
    which did its own is_file()/stat() and one even did an open()+read)
    was eating tens of seconds before any worker started.

    Mbox detection now relies on the .mbox extension only. That covers
    everything in our actual corpus (Nazario mboxes carry the suffix);
    if a user drops an extensionless mbox in manual-supplement/, they'll
    need to rename it to .mbox or it'll be treated as a single-message
    eml.
    """
    suf = p.suffix.lower()
    try:
        st = p.stat()
    except OSError:
        return None
    # stat.S_ISREG check via st_mode — avoids re-stat from p.is_file()
    import stat as _stat
    if not _stat.S_ISREG(st.st_mode):
        return None
    if suf == ".mbox":
        return "mbox"
    if suf in {".eml", ".msg"}:
        return "eml"
    if suf in _SKIP_SUFFIXES:
        return None
    # SpamAssassin / Enron store messages as plain extensionless files.
    # Filter by size only — the email parser will reject anything that
    # isn't actually a message.
    return "eml" if 64 <= st.st_size <= 2 * 1024 * 1024 else None


def iter_messages_from_file(p: Path) -> Iterable[email.message.Message]:
    """Yield Message objects from an mbox or single-message file."""
    try:
        if p.suffix.lower() == ".mbox":
            mb = mailbox.mbox(str(p), factory=lambda f: email.message_from_binary_file(
                f, policy=email.policy.default))
            for msg in mb:
                yield msg
            return
        with p.open("rb") as f:
            data = f.read()
        msg = email.message_from_bytes(data, policy=email.policy.default)
        # Refuse messages with no MIME structure — they're probably
        # not real emails (Enron, for instance, has some stub files).
        if msg.get_content_type() or msg.is_multipart():
            yield msg
    except (OSError, email.errors.MessageError):
        return
    except Exception:
        # Defensive: malformed mboxes can throw anything. Skip silently.
        return


def extract_parts(msg: email.message.Message) -> Iterable[tuple[str, bytes]]:
    """Yield (kind, bytes) tuples for each body-bearing leaf MIME part.

    kind is 'text' or 'html'; bytes is the decoded body of the part.
    """
    if msg.is_multipart():
        for part in msg.walk():
            if part.is_multipart():
                continue
            yield from _part_body(part)
    else:
        yield from _part_body(msg)


def _part_body(part: email.message.Message) -> Iterable[tuple[str, bytes]]:
    ctype = (part.get_content_type() or "").lower()
    if ctype == "text/plain":
        kind = "text"
    elif ctype == "text/html":
        kind = "html"
    else:
        return
    try:
        body = part.get_payload(decode=True)
    except Exception:
        return
    if not body:
        return
    # Try to decode to str via the declared charset; fall back to
    # utf-8-with-replace. We keep bytes in the output (matching what
    # the runtime cache stores), but normalizing CR/LF helps the
    # dictionary find more common substrings.
    charset = part.get_content_charset() or "utf-8"
    try:
        text = body.decode(charset, errors="replace")
    except (LookupError, AttributeError):
        text = body.decode("utf-8", errors="replace")
    # Normalize line endings — real cached bodies are stored as
    # received but newsletter senders are inconsistent (CRLF, LF, CR);
    # collapsing to LF concentrates dict bytes on the actual chrome
    # rather than on alternating CRLF runs.
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    out = text.encode("utf-8")
    if len(out) > PER_SAMPLE_CAP:
        out = out[:PER_SAMPLE_CAP]
    if len(out) < MIN_SAMPLE_BYTES:
        return
    yield kind, out


def hash_name(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()[:16]


# Per-file output cap. A single Enron user folder or LKML mbox can hold
# tens of thousands of messages; without a cap one file can dominate
# the worker's return list and balloon IPC pickle size. 5000 samples
# per file is plenty to feed any per-source cap downstream.
PER_FILE_SAMPLE_CAP = 5000


# Top-level worker function — must be picklable for ProcessPoolExecutor.
# Returns (src_name, file_path_str, samples) where samples is a list of
# (kind, hash, bytes) tuples. Main process applies global dedup,
# per-source caps, and per-kind byte budgets before writing.
def process_file(task: tuple[str, str]) -> tuple[str, str, list[tuple[str, str, bytes]]]:
    src_name, path_str = task
    p = Path(path_str)
    out: list[tuple[str, str, bytes]] = []
    for msg in iter_messages_from_file(p):
        if len(out) >= PER_FILE_SAMPLE_CAP:
            break
        for kind, data in extract_parts(msg):
            out.append((kind, hash_name(data), data))
            if len(out) >= PER_FILE_SAMPLE_CAP:
                break
    return src_name, path_str, out


def main() -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--corpus-dir", default=str(here / "corpus-cache"))
    ap.add_argument("--out",         default=str(here / "samples"))
    ap.add_argument("--max-samples-per-source", type=int, default=20000,
                    help="cap on samples per top-level corpus directory")
    ap.add_argument("--budget-mib", type=int, default=DEFAULT_BUDGET_MIB,
                    help=f"total sample-byte budget in MiB "
                         f"(default: {DEFAULT_BUDGET_MIB}; "
                         f"~70%% HTML / ~30%% plaintext split). zstd's "
                         f"sample-volume recommendation is ~100x the "
                         f"target dict size, so 11+ MiB covers a 110 "
                         f"KiB dict; defaults to generous-headroom.")
    ap.add_argument("--workers", type=int,
                    default=min(os.cpu_count() or 4, 8),
                    help="parallel worker processes (default: min(CPU, 8))")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    # Per-kind byte budgets derived from --budget-mib. Computed here
    # (not at module scope) so the CLI flag actually changes behavior.
    total_budget = args.budget_mib * 1024 * 1024
    target_html_bytes = int(total_budget * HTML_FRACTION)
    target_text_bytes = int(total_budget * TEXT_FRACTION)
    print(f"→ sample budget: {args.budget_mib} MiB total "
          f"({target_html_bytes // (1024*1024)} MiB HTML / "
          f"{target_text_bytes // (1024*1024)} MiB text)", flush=True)

    corpus = Path(args.corpus_dir)
    if not corpus.is_dir():
        print(f"corpus-dir not found: {corpus}", file=sys.stderr)
        return 1

    out = Path(args.out)
    (out / "text").mkdir(parents=True, exist_ok=True)
    (out / "html").mkdir(parents=True, exist_ok=True)

    # Top-level subdirs of corpus-cache/ are the "sources" we balance
    # against per-source caps. Anything dropped at the root counts as
    # its own pseudo-source.
    sources = sorted(p for p in corpus.iterdir() if p.is_dir())

    # Build the full work list up front. Each task is (src_name, path).
    # Workers stay simple; they don't need any directory knowledge.
    # Print status as we go so the user isn't staring at a blank screen
    # during the 500k-file Enron walk.
    print(f"→ scanning corpus under {corpus}...", flush=True)
    walk_t0 = time.monotonic()
    tasks: list[tuple[str, str]] = []
    files_per_source: dict[str, int] = {}
    for src in sources:
        n = 0
        for p in src.rglob("*"):
            kind = classify_file(p)
            if kind is not None:
                tasks.append((src.name, str(p)))
                n += 1
        files_per_source[src.name] = n
        if args.verbose:
            print(f"  [{src.name}] {n} candidate file(s)", flush=True)

    if not tasks:
        print(f"no candidate files found under {corpus}", file=sys.stderr)
        return 1

    print(f"→ scan done in {time.monotonic() - walk_t0:.1f}s — "
          f"processing {len(tasks)} files across {len(sources)} sources "
          f"on {args.workers} workers", flush=True)

    seen_hashes: set[str] = set()
    counts = {"text": 0, "html": 0}
    bytes_total = {"text": 0, "html": 0}
    per_source: dict[str, dict[str, int]] = {
        s.name: {"text": 0, "html": 0, "files": 0} for s in sources
    }
    src_caps_hit: set[str] = set()

    t0 = time.monotonic()
    processed_files = 0

    # ProcessPoolExecutor.map preserves submission order, which keeps
    # the budget-enforcement decisions deterministic across runs
    # (same corpus → same dict). Throughput-wise we still get full
    # parallelism — map only blocks on per-result yield order, not
    # task execution order.
    #
    # chunksize=4 amortizes per-task IPC overhead for the many small
    # .eml/Enron files SpamAssassin and Enron produce, without
    # serializing mboxes (each one is its own task regardless).
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        for src_name, path_str, samples in ex.map(
                process_file, tasks, chunksize=4):
            processed_files += 1
            per_source[src_name]["files"] += 1

            if src_name in src_caps_hit:
                # Cap was reached for this source before this file
                # came back; drop its samples. Worker already paid
                # the parse cost — that's the price of streaming-
                # ish parallelism, but parse cost is dominated by
                # large mboxes which we want anyway.
                pass
            else:
                src_cap = args.max_samples_per_source
                for kind, h, data in samples:
                    # Honor per-kind byte budgets to enforce the
                    # ~70% HTML / ~30% plaintext target.
                    budget = (target_html_bytes if kind == "html"
                              else target_text_bytes)
                    if bytes_total[kind] >= budget:
                        continue
                    if h in seen_hashes:
                        continue
                    seen_hashes.add(h)

                    sub = out / kind / src_name
                    sub.mkdir(parents=True, exist_ok=True)
                    ext = ".html" if kind == "html" else ".txt"
                    (sub / f"{h}{ext}").write_bytes(data)

                    counts[kind] += 1
                    bytes_total[kind] += len(data)
                    per_source[src_name][kind] += 1

                    if (per_source[src_name]["text"]
                        + per_source[src_name]["html"]) >= src_cap:
                        src_caps_hit.add(src_name)
                        break

            # Progress every ~5% or every 500 files, whichever
            # comes first. Keeps stderr quiet on small runs.
            step = max(1, min(500, len(tasks) // 20))
            if args.verbose and processed_files % step == 0:
                dt = time.monotonic() - t0
                pct = 100.0 * processed_files / len(tasks)
                print(f"  [{processed_files:6d}/{len(tasks)}] "
                      f"{pct:5.1f}% • text={counts['text']} "
                      f"html={counts['html']} • {dt:.1f}s elapsed",
                      file=sys.stderr)

    # Enforce the target ratio when one kind is scarce. Without this,
    # if HTML undershoots its budget (the corpus runs out of HTML
    # before the limit) but text fills its own budget independently,
    # the corpus ends up plaintext-heavy. That distorts the trainer's
    # held-out test: plaintext compresses worse with a dict, so the
    # average ratio inflates purely from composition.
    #
    # Cap text bytes to bytes_total["html"] × TEXT/HTML so the final
    # corpus matches the configured fraction even when HTML is scarce.
    if bytes_total["html"] > 0:
        max_text_bytes = int(
            bytes_total["html"] * TEXT_FRACTION / HTML_FRACTION)
        if bytes_total["text"] > max_text_bytes:
            text_root = out / "text"
            # Gather every text file with its size and source. Sort by
            # filename (hash) for deterministic trim across runs.
            text_files: list[tuple[str, Path, str, int]] = []
            for src_dir in sorted(p for p in text_root.iterdir() if p.is_dir()):
                src = src_dir.name
                for f in src_dir.iterdir():
                    if f.is_file():
                        try:
                            sz = f.stat().st_size
                        except OSError:
                            continue
                        text_files.append((f.name, f, src, sz))
            text_files.sort(key=lambda t: t[0])

            running = 0
            kept = 0
            dropped = 0
            for _name, path, src, sz in text_files:
                if running + sz <= max_text_bytes:
                    running += sz
                    kept += 1
                else:
                    try:
                        path.unlink()
                    except OSError:
                        continue
                    dropped += 1
                    bytes_total["text"] -= sz
                    counts["text"] -= 1
                    per_source[src]["text"] -= 1
            print(f"→ ratio trim: kept {kept} text samples "
                  f"({running / (1024*1024):.2f} MiB), dropped {dropped} "
                  f"to maintain {int(TEXT_FRACTION*100)}/{int(HTML_FRACTION*100)} "
                  f"ratio against {bytes_total['html'] / (1024*1024):.2f} MiB HTML",
                  flush=True)

    if args.verbose:
        for s in sources:
            ps = per_source[s.name]
            print(f"[{s.name}] kept text={ps['text']} html={ps['html']} "
                  f"from files={ps['files']}")

    # Write a manifest so the trainer (and humans) can see what landed
    # without re-walking the directory.
    manifest = {
        "samples": {
            "text": counts["text"],
            "html": counts["html"],
            "total": counts["text"] + counts["html"],
        },
        "bytes": {
            "text": bytes_total["text"],
            "html": bytes_total["html"],
            "total": bytes_total["text"] + bytes_total["html"],
        },
        "per_sample_cap": PER_SAMPLE_CAP,
        "min_sample_bytes": MIN_SAMPLE_BYTES,
        "balance_target": {
            "html_fraction": HTML_FRACTION,
            "text_fraction": TEXT_FRACTION,
            "total_budget_bytes": total_budget,
        },
        "per_source": per_source,
    }
    (out / "index.json").write_text(json.dumps(manifest, indent=2))

    total_mb = (bytes_total["text"] + bytes_total["html"]) / (1024 * 1024)
    print(f"\n=== extract_samples.py summary ===")
    print(f"Output:      {out}")
    print(f"text/plain:  {counts['text']:6d} samples, "
          f"{bytes_total['text'] / (1024*1024):.2f} MiB")
    print(f"text/html:   {counts['html']:6d} samples, "
          f"{bytes_total['html'] / (1024*1024):.2f} MiB")
    print(f"total:       {counts['text'] + counts['html']:6d} samples, "
          f"{total_mb:.2f} MiB")
    if counts["text"] + counts["html"] == 0:
        print("\n! No samples extracted. Did fetch_corpus.sh complete?")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
