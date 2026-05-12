#!/usr/bin/env python3
"""
seed_coverage.py — Audit how often each seed pattern actually appears
in the extracted training samples. Helps identify dead weight in
seed_content.txt — bytes that occupy slots in the dict content tail
but never substitute at compression time.

Usage:
    python3 seed_coverage.py                          # default summary
    python3 seed_coverage.py --show-all               # every pattern
    python3 seed_coverage.py --threshold 0.5          # tighter cutoff
    python3 seed_coverage.py --json > coverage.json   # machine-readable
    python3 seed_coverage.py --kind html              # only HTML samples

Reads:
    seed_content.txt   (sections marked with `## SectionName` lines)
    samples/text/...   (extracted plaintext bodies)
    samples/html/...   (extracted HTML bodies)

Reports per pattern:
    - hit_count: how many samples contain the pattern as a substring
    - hit_rate:  hit_count / total_samples_scanned

Reports per section:
    - patterns      : count of patterns in the section
    - avg_hit_rate  : mean hit rate across the section's patterns
    - dead_patterns : count whose hit_rate < --threshold

Dead-weight patterns aren't bugs — they're just slots in the seed that
the dict could use more profitably. Trim them if you want a tighter
seed, or leave them as future-proofing if the dict size isn't pressed.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path


def parse_seed(path: Path) -> list[tuple[str, list[str]]]:
    """Return [(section_name, [pattern, ...]), ...].

    Sections are introduced by lines beginning with "## ". Anything
    before the first "## " line is treated as preamble and dropped.
    Blank lines are dropped (the trainer keeps them for the dict tail,
    but they're not pattern candidates).
    """
    sections: list[tuple[str, list[str]]] = []
    current_name: str | None = None
    current_patterns: list[str] = []

    for raw in path.read_text(encoding="utf-8").splitlines():
        if raw.startswith("## "):
            if current_name is not None:
                sections.append((current_name, current_patterns))
            current_name = raw[3:].strip()
            current_patterns = []
            continue
        if current_name is None:
            continue  # preamble
        stripped = raw.strip()
        if not stripped:
            continue
        # Preserve leading/trailing whitespace inside the line — some
        # patterns rely on a leading " wrote:" / trailing " wrote:>" style.
        current_patterns.append(raw)

    if current_name is not None:
        sections.append((current_name, current_patterns))
    return sections


def load_all_samples(samples_dir: Path,
                     kind_filter: str) -> list[tuple[str, str]]:
    """Return [(relative_path, content_str), ...] for matching samples.

    kind_filter: "text", "html", or "both".
    """
    out: list[tuple[str, str]] = []
    if not samples_dir.is_dir():
        return out
    for p in samples_dir.rglob("*"):
        if not p.is_file():
            continue
        if p.name == "index.json":
            continue
        # Categorize by subtree.
        ps = str(p)
        is_html = "/html/" in ps or "\\html\\" in ps
        is_text = "/text/" in ps or "\\text\\" in ps
        if kind_filter == "html" and not is_html:
            continue
        if kind_filter == "text" and not is_text:
            continue
        try:
            content = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = str(p.relative_to(samples_dir))
        out.append((rel, content))
    return out


def count_hits_worker(task: tuple[list[str], list[tuple[str, str]]]
                      ) -> list[int]:
    """For one batch of samples, return per-pattern hit counts."""
    patterns, samples = task
    counts = [0] * len(patterns)
    for _path, content in samples:
        for i, pat in enumerate(patterns):
            if pat in content:
                counts[i] += 1
    return counts


def parse_args() -> argparse.Namespace:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seed", default=str(here / "seed_content.txt"))
    ap.add_argument("--samples-dir", default=str(here / "samples"))
    ap.add_argument("--kind", choices=["both", "text", "html"], default="both",
                    help="restrict to plaintext or HTML samples")
    ap.add_argument("--threshold", type=float, default=1.0,
                    help="dead-weight cutoff (percent of samples). "
                         "Patterns hitting fewer than this fraction of "
                         "samples are flagged.")
    ap.add_argument("--show-all", action="store_true",
                    help="print every pattern, not just top + dead-weight")
    ap.add_argument("--workers", type=int,
                    default=min(os.cpu_count() or 4, 8))
    ap.add_argument("--json", action="store_true",
                    help="emit machine-readable JSON instead of text")
    return ap.parse_args()


def truncate(s: str, n: int = 72) -> str:
    s = s.replace("\t", "\\t").replace("\n", "\\n")
    return s if len(s) <= n else s[: n - 3] + "..."


def main() -> int:
    args = parse_args()
    seed_path = Path(args.seed)
    samples_dir = Path(args.samples_dir)

    sections = parse_seed(seed_path)
    flat: list[tuple[str, str]] = []
    for sec_name, patterns in sections:
        for pat in patterns:
            flat.append((sec_name, pat))
    if not flat:
        print(f"no patterns parsed from {seed_path} — does it have ## headers?",
              file=sys.stderr)
        return 1

    samples = load_all_samples(samples_dir, args.kind)
    if not samples:
        print(f"no samples found under {samples_dir} (kind={args.kind})",
              file=sys.stderr)
        return 1

    patterns_only = [p for _, p in flat]
    n_workers = max(1, args.workers)
    batch = max(1, (len(samples) + n_workers - 1) // n_workers)
    batches = [samples[i : i + batch]
               for i in range(0, len(samples), batch)]

    if not args.json:
        print(f"→ {len(flat)} patterns across {len(sections)} sections, "
              f"{len(samples)} samples (kind={args.kind}), "
              f"{n_workers} workers")

    t0 = time.monotonic()
    with ProcessPoolExecutor(max_workers=n_workers) as ex:
        partial = list(ex.map(
            count_hits_worker,
            [(patterns_only, b) for b in batches]))
    totals = [0] * len(flat)
    for pc in partial:
        for i, c in enumerate(pc):
            totals[i] += c
    dt = time.monotonic() - t0
    if not args.json:
        print(f"→ scan complete in {dt:.1f}s")

    n_samples = len(samples)
    # Keep threshold as a float so e.g. 60 × 1% = 0.6 still triggers
    # on 0-hit patterns. (int() would round 0.6 → 0 and nothing dead.)
    threshold_count = n_samples * args.threshold / 100.0
    # Aggregate by section, preserving original section order.
    by_section: dict[str, dict] = {}
    section_order: list[str] = []
    for sec, pat, hits in zip(
            (s for s, _ in flat),
            (p for _, p in flat),
            totals):
        if sec not in by_section:
            by_section[sec] = {"patterns": [], "hits_total": 0}
            section_order.append(sec)
        rate = hits / n_samples if n_samples else 0.0
        by_section[sec]["patterns"].append({
            "pattern": pat, "hits": hits, "rate": rate,
        })
        by_section[sec]["hits_total"] += hits

    # ----- JSON output path -----
    if args.json:
        out = {
            "samples_scanned": n_samples,
            "patterns_total":  len(flat),
            "threshold_pct":   args.threshold,
            "scan_seconds":    dt,
            "sections": [
                {
                    "name": sec,
                    "patterns_total": len(by_section[sec]["patterns"]),
                    "dead_patterns":  sum(
                        1 for p in by_section[sec]["patterns"]
                        if p["hits"] < threshold_count),
                    "avg_hit_rate":   (
                        sum(p["rate"] for p in by_section[sec]["patterns"])
                        / max(1, len(by_section[sec]["patterns"]))),
                    "patterns": by_section[sec]["patterns"],
                }
                for sec in section_order
            ],
        }
        json.dump(out, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0

    # ----- Text output path -----
    print(f"\n=== Seed coverage report ===")
    print(f"Samples scanned:    {n_samples} ({args.kind})")
    print(f"Patterns evaluated: {len(flat)}")
    print(f"Dead-weight cutoff: < {args.threshold}% "
          f"({threshold_count} samples)\n")

    total_dead = 0
    for sec in section_order:
        items = sorted(by_section[sec]["patterns"],
                       key=lambda x: x["hits"], reverse=True)
        avg_rate = (sum(p["rate"] for p in items) / max(1, len(items)))
        dead = [p for p in items if p["hits"] < threshold_count]
        total_dead += len(dead)
        print(f"## {sec}")
        print(f"   {len(items)} patterns • avg hit rate {avg_rate*100:5.1f}% "
              f"• dead {len(dead)}/{len(items)}")

        if args.show_all:
            for p in items:
                mark = "○" if p["hits"] < threshold_count else "●"
                print(f"   {mark} {p['hits']:6d} ({p['rate']*100:5.1f}%)  "
                      f"{truncate(p['pattern'])}")
        else:
            # Top performers — patterns over the threshold, sorted by
            # hits desc. Cap at 3. If a section has no patterns over
            # the threshold, top is empty (skip the "top" subhead).
            live = [p for p in items if p["hits"] >= threshold_count]
            top = live[:3]
            for p in top:
                print(f"   ● {p['hits']:6d} ({p['rate']*100:5.1f}%)  "
                      f"{truncate(p['pattern'])}")
            # Dead-weight — anything below threshold, excluding items
            # already shown in `top` (only relevant if threshold is
            # high enough that top patterns also count as dead; rare
            # but possible).
            top_patterns = {p["pattern"] for p in top}
            dead_to_show = [p for p in dead
                            if p["pattern"] not in top_patterns]
            if dead_to_show:
                shown = min(5, len(dead_to_show))
                for p in dead_to_show[:shown]:
                    print(f"   ○ {p['hits']:6d} ({p['rate']*100:5.1f}%)  "
                          f"{truncate(p['pattern'])}")
                remaining = len(dead_to_show) - shown
                if remaining > 0:
                    print(f"     ... and {remaining} more dead "
                          f"(--show-all to list)")
        print()

    pct = 100.0 * total_dead / len(flat) if flat else 0.0
    print(f"Total dead-weight: {total_dead}/{len(flat)} "
          f"({pct:.1f}%) patterns hit < {args.threshold}% of samples")
    print("(Dead-weight isn't a bug — it's a hint about trimming the seed "
          "if you want tighter byte allocation.)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
