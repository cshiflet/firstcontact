#!/usr/bin/env python3
"""
fetch_newsletters.py — fetches a small sample of public newsletter
web mirrors via their RSS/Atom feeds.

Why: SpamAssassin and Enron only cover 2001-2005 HTML email patterns.
Modern email chrome (current ESP class names, mobile-responsive meta
tags, dark-mode declarations) lives in today's newsletter HTML. The
public web copies these newsletters expose via RSS are the freshest
samples available without scraping.

Politeness:
  - 2-second delay between feed fetches
  - Honest User-Agent identifying the tool
  - Checks robots.txt before each fetch
  - Per-feed post cap (default 15)
  - No login-walled or paywalled content

Output: corpus-cache/newsletter-web/<host>/<slug>.eml

Each post is wrapped in a minimal MIME envelope (one header,
Content-Type: text/html) so extract_samples.py picks them up
identically to any other email source.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
import time
from pathlib import Path
from urllib import parse as urlparse
from urllib import request as urlrequest
from urllib import robotparser
from urllib.error import HTTPError, URLError
from xml.etree import ElementTree as ET

USER_AGENT = (
    "firstcontact-bundled-dict-trainer/1 "
    "(+https://github.com/cshiflet/firstcontact; "
    "training a public-corpus compression dictionary, not republishing)"
)
DEFAULT_DELAY_SEC = 2.0
FETCH_TIMEOUT_SEC = 30
MIN_BODY_BYTES = 512


def parse_args() -> argparse.Namespace:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--feeds-file",
                    default=str(here / "newsletter-feeds.txt"),
                    help="text file with one feed URL per line")
    ap.add_argument("--out",
                    default=str(here / "corpus-cache" / "newsletter-web"),
                    help="directory under corpus-cache to populate")
    ap.add_argument("--max-posts-per-feed", type=int, default=15)
    ap.add_argument("--delay", type=float, default=DEFAULT_DELAY_SEC,
                    help="seconds between feed fetches (politeness)")
    ap.add_argument("--verbose", "-v", action="store_true")
    return ap.parse_args()


def read_feeds(path: Path) -> list[str]:
    if not path.is_file():
        return []
    out: list[str] = []
    for line in path.read_text().splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        out.append(s)
    return out


def robots_allows(url: str) -> bool:
    """Return True if robots.txt permits fetching `url`.

    Missing or unreachable robots.txt → allow (RFC 9309 default).
    """
    parts = urlparse.urlparse(url)
    if not parts.scheme or not parts.netloc:
        return False
    robots_url = f"{parts.scheme}://{parts.netloc}/robots.txt"
    rp = robotparser.RobotFileParser()
    rp.set_url(robots_url)
    try:
        rp.read()
    except Exception:
        return True
    try:
        return rp.can_fetch(USER_AGENT, url)
    except Exception:
        return True


def http_get(url: str) -> bytes:
    req = urlrequest.Request(url, headers={"User-Agent": USER_AGENT})
    with urlrequest.urlopen(req, timeout=FETCH_TIMEOUT_SEC) as resp:
        return resp.read()


_SLUG_BAD = re.compile(r"[^A-Za-z0-9_-]+")


def slug_from_url(url: str) -> str:
    if not url:
        return ""
    path = urlparse.urlparse(url).path
    last = path.rstrip("/").rsplit("/", 1)[-1]
    s = _SLUG_BAD.sub("-", last).strip("-")
    return s[:80]


# Common feed namespaces — RSS 2.0 + Atom + RDF feeds all show up.
_NS = {
    "content": "http://purl.org/rss/1.0/modules/content/",
    "atom":    "http://www.w3.org/2005/Atom",
    "dc":      "http://purl.org/dc/elements/1.1/",
}


def _strip_ns(tag: str) -> str:
    """Return the local part of an ElementTree tag like '{ns}name'."""
    return tag.rsplit("}", 1)[-1] if "}" in tag else tag


def parse_feed_items(xml_bytes: bytes,
                     max_posts: int) -> list[tuple[str, str]]:
    """Return (slug, html_body) pairs from an RSS or Atom feed.

    Picks <content:encoded> first, then <description>, then
    <atom:content>. Any item whose body isn't HTML-ish (no '<') or is
    shorter than MIN_BODY_BYTES is dropped — those are usually
    summary-only feeds that don't include the full post.
    """
    try:
        root = ET.fromstring(xml_bytes)
    except ET.ParseError:
        return []

    out: list[tuple[str, str]] = []

    # RSS 2.0 / RDF — <item> elements anywhere in the tree.
    items = [el for el in root.iter() if _strip_ns(el.tag) == "item"]
    if items:
        for item in items[:max_posts * 4]:  # over-pull; we filter
            link = ""
            body = ""
            for child in item:
                tag = _strip_ns(child.tag)
                if tag == "link" and child.text:
                    link = child.text.strip()
                elif tag == "encoded" and child.text:
                    body = child.text
                elif tag == "description" and not body and child.text:
                    body = child.text
            if not body or len(body) < MIN_BODY_BYTES or "<" not in body:
                continue
            slug = slug_from_url(link) or _hash_slug(body)
            out.append((slug, body))
            if len(out) >= max_posts:
                break
        return out

    # Atom — <entry> with <content>.
    entries = [el for el in root.iter() if _strip_ns(el.tag) == "entry"]
    for entry in entries[:max_posts * 4]:
        link = ""
        body = ""
        for child in entry:
            tag = _strip_ns(child.tag)
            if tag == "link":
                href = child.get("href")
                if href and not link:
                    link = href
            elif tag == "content" and child.text:
                body = child.text
        if not body or len(body) < MIN_BODY_BYTES or "<" not in body:
            continue
        slug = slug_from_url(link) or _hash_slug(body)
        out.append((slug, body))
        if len(out) >= max_posts:
            break
    return out


def _hash_slug(body: str) -> str:
    return hashlib.sha1(body.encode("utf-8")).hexdigest()[:12]


def write_as_eml(out_dir: Path, slug: str, html_body: str) -> Path:
    """Wrap an HTML body in a minimal MIME envelope and write to disk.

    extract_samples.py walks corpus-cache/ looking for .eml-shaped
    files and runs them through email.message_from_bytes; the wrapper
    is the smallest thing that parses cleanly as an HTML-only message.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    # Strip any leading XML/CDATA wrappers that some feeds include.
    body = html_body.strip()
    envelope = (
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: 8bit\r\n"
        "\r\n"
    )
    path = out_dir / f"{slug}.eml"
    path.write_bytes(envelope.encode("ascii") + body.encode("utf-8"))
    return path


def main() -> int:
    args = parse_args()

    feeds_path = Path(args.feeds_file)
    feeds = read_feeds(feeds_path)
    if not feeds:
        print(f"no feeds enabled in {feeds_path}", file=sys.stderr)
        return 0

    out_root = Path(args.out)
    out_root.mkdir(parents=True, exist_ok=True)

    fetched_feeds = 0
    saved_posts = 0
    errors = 0
    skipped_by_robots = 0

    for feed_url in feeds:
        if args.verbose:
            print(f"→ {feed_url}")
        if not robots_allows(feed_url):
            print(f"  skip: robots.txt disallows {feed_url}")
            skipped_by_robots += 1
            continue
        try:
            data = http_get(feed_url)
        except (HTTPError, URLError, TimeoutError) as e:
            print(f"  fetch failed: {e}")
            errors += 1
            time.sleep(args.delay)
            continue

        items = parse_feed_items(data, args.max_posts_per_feed)
        if not items:
            print(f"  no usable items in feed (summary-only or empty)")
            time.sleep(args.delay)
            continue

        host = urlparse.urlparse(feed_url).netloc.replace(":", "_")
        host_dir = out_root / host
        for slug, body in items:
            try:
                write_as_eml(host_dir, slug, body)
                saved_posts += 1
            except OSError as e:
                print(f"  write failed {slug}: {e}")
                errors += 1

        fetched_feeds += 1
        if args.verbose:
            print(f"  saved {len(items)} posts to {host_dir}")

        time.sleep(args.delay)

    print(f"\n=== fetch_newsletters.py summary ===")
    print(f"Output:         {out_root}")
    print(f"Feeds fetched:  {fetched_feeds} / {len(feeds)}")
    print(f"Posts saved:    {saved_posts}")
    print(f"Errors:         {errors}")
    print(f"Skipped (robots): {skipped_by_robots}")

    if saved_posts == 0:
        print("\n! No posts saved. Check feed URLs and network connectivity.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
