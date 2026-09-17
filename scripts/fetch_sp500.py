#!/usr/bin/env python3
"""Fetches the S&P 500 membership history into the local cache.

Source: https://github.com/fja05680/sp500, which reconstructs the index's
constituents back to 1996 as a file of (date, comma-separated tickers)
snapshots. Free, no key, no account.

WHAT IT CAN AND CANNOT TELL US

It is ticker-keyed, and a ticker is not an identity. The file cannot express
that FB became META (one company, two names) or that a ticker dropped in 2003
and reassigned in 2011 is two companies sharing a string. Both have to be
inferred, and the inference is in the C++ side -- with every decision it makes
written to docs/IDENTITY_DECISIONS.md, because the threshold it uses is a
judgment call rather than a derived constant.

Renames in particular are *not* inferable from this file: a rename looks
exactly like one name leaving and another arriving. They have to be declared by
hand. CRSP's PERMNO supplies identity directly and makes all of this go away.

    python scripts/fetch_sp500.py
    python scripts/fetch_sp500.py --status

Nothing this writes is committed: data/cache/ is gitignored.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CACHE = REPO / "data" / "cache" / "universe"
TARGET = CACHE / "sp500_history.csv"

CONTENTS_API = "https://api.github.com/repos/fja05680/sp500/contents/"
USER_AGENT = "MarketScan/0.1 (research tool; contact via repository)"


def find_history_file(timeout: float) -> tuple[str, str]:
    """The newest historical-components file, by name.

    Located rather than hard-coded: the filename carries the date it was last
    regenerated, so a literal URL rots the first time the source is updated.
    """
    request = urllib.request.Request(CONTENTS_API, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        entries = json.loads(response.read().decode("utf-8"))

    candidates = [
        entry for entry in entries
        if entry.get("type") == "file"
        and entry.get("name", "").lower().endswith(".csv")
        and "historical" in entry.get("name", "").lower()
        and "component" in entry.get("name", "").lower()
    ]
    if not candidates:
        raise RuntimeError(
            "no historical-components CSV in the repository listing; the source has been "
            "reorganised and this script needs updating")
    # The repository carries both the original file and an "(Updated)" one that
    # runs to the present. Prefer the updated file; a sort by name would pick
    # the other, because "(" sorts before "." and the plain name lands last.
    updated = [e for e in candidates if "updated" in e["name"].lower()]
    best = (updated or candidates)[0]
    return best["name"], best["download_url"]


def status() -> int:
    if not TARGET.exists():
        print(f"nothing cached at {TARGET}")
        return 0
    lines = TARGET.read_text(encoding="utf-8").splitlines()
    dated = [line for line in lines if line[:4].isdigit()]
    print(f"{TARGET}")
    print(f"  {len(dated):,} snapshots")
    if dated:
        print(f"  {dated[0][:10]} .. {dated[-1][:10]}")
        print(f"  {len(dated[-1].split(',')) - 1} members on the last snapshot")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--refresh", action="store_true", help="re-fetch even if cached")
    parser.add_argument("--status", action="store_true")
    args = parser.parse_args()

    if args.status:
        return status()

    if TARGET.exists() and not args.refresh:
        print(f"already cached at {TARGET}; --refresh to fetch again")
        return status()

    try:
        name, url = find_history_file(args.timeout)
        print(f"fetching {name}")
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(request, timeout=args.timeout) as response:
            body = response.read().decode("utf-8", errors="replace")
    except (urllib.error.URLError, TimeoutError, OSError, RuntimeError, ValueError) as error:
        print(f"could not fetch the membership history: {error}", file=sys.stderr)
        print("\nThis is the only network step in the project and everything else works "
              "without it.\nThe universe machinery is tested against synthetic fixtures; "
              "this is what makes it\npoint at the real index.", file=sys.stderr)
        return 1

    CACHE.mkdir(parents=True, exist_ok=True)
    # Written to a temporary and renamed, so an interrupted fetch never leaves
    # a half-written file the next run would mistake for a complete one.
    temporary = TARGET.with_suffix(".csv.part")
    temporary.write_text(body, encoding="utf-8")
    temporary.replace(TARGET)

    print(f"wrote {TARGET}")
    return status()


if __name__ == "__main__":
    raise SystemExit(main())
