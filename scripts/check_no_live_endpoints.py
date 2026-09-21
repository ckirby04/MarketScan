#!/usr/bin/env python3
"""Fails if any broker hostname outside the paper allowlist appears anywhere.

WHY AN ALLOWLIST RATHER THAN A BLOCKLIST

The live-money trading host is the paper host with a prefix removed. So:

- searching for the live hostname also matches the paper hostname, because the
  first is a substring of the second;
- searching for it while excluding the paper hostname misses any live URL
  spelled a little differently, and the whole point of a guard is the case
  nobody anticipated.

So this does not look for a forbidden string. It finds every host in the broker's
domain and requires each one to be on a list of hosts that are allowed. An
endpoint nobody has thought of fails, which is the direction that keeps working.

It also means the forbidden hostname never has to be written down -- not in this
file, not in a test, not in a comment. A guard that has to name the thing it
forbids has put the thing it forbids in the repository.

WHAT IT SCANS

The source tree, and any built binaries handed to it. Binaries matter because a
string can reach one through a header, a generated file, or a dependency without
appearing in any source file a reviewer would read.

    python scripts/check_no_live_endpoints.py
    python scripts/check_no_live_endpoints.py --binaries out/build/release/bin
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

DOMAIN = "alpaca.markets"

# Kept in step with include/marketscan/live/endpoints.hpp; the C++ guard asserts
# the two agree, so this list cannot drift without a test failing.
ALLOWED = {
    "paper-api.alpaca.markets",
    "data.alpaca.markets",
    "stream.data.alpaca.markets",
    # The apex domain. Not a host anything connects to -- it is the constant the
    # guard itself is built from, and it appears in every file and binary that
    # carries the guard. Permitting it costs nothing, because the live trading
    # host carries a prefix and is therefore a different string.
    DOMAIN,
}

HOST = re.compile(r"[A-Za-z0-9][A-Za-z0-9.\-]*" + re.escape(DOMAIN))

SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".c", ".py", ".md", ".txt", ".json", ".yml",
                   ".yaml", ".ps1", ".cmake", ".toml", ".ini", ".csv"}

SELF = Path(__file__).name

SKIP_DIRS = {"out", "build", ".git", "vcpkg_installed", "__pycache__", ".vs"}

HOST_CHARS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-")


def scan_text(path: Path) -> list[tuple[int, str]]:
    bad: list[tuple[int, str]] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return bad
    for number, line in enumerate(text.splitlines(), start=1):
        for match in HOST.finditer(line):
            host = match.group(0).lower().strip(".")
            if host not in ALLOWED:
                bad.append((number, host))
    return bad


def scan_binary(path: Path) -> list[str]:
    """Every host in the domain that appears in a binary's bytes."""
    try:
        blob = path.read_bytes()
    except OSError:
        return []
    found: list[str] = []
    needle = DOMAIN.encode()
    start = 0
    while True:
        at = blob.find(needle, start)
        if at < 0:
            break
        begin = at
        while begin > 0 and chr(blob[begin - 1]) in HOST_CHARS:
            begin -= 1
        host = blob[begin:at + len(needle)].decode("ascii", errors="replace").lower().strip(".")
        if host not in ALLOWED:
            found.append(host)
        start = at + len(needle)
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binaries", type=Path, default=None,
                        help="a directory of built binaries to scan as well")
    args = parser.parse_args(argv)

    failures: list[str] = []
    scanned = 0

    for path in sorted(REPO.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if SKIP_DIRS & set(path.parts):
            continue
        if path.name == SELF:
            continue
        scanned += 1
        for number, host in scan_text(path):
            failures.append(
                f"{path.relative_to(REPO)}:{number}: host not on the allowlist: {host}")

    binaries = 0
    if args.binaries is not None:
        for path in sorted(args.binaries.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in {".exe", ".dll"}:
                continue
            binaries += 1
            for host in scan_binary(path):
                failures.append(f"{path}: host not on the allowlist in the binary: {host}")

    print(f"scanned {scanned} source files and {binaries} binaries")
    print("allowed hosts: " + ", ".join(sorted(ALLOWED)))

    if failures:
        print("\nFAILED -- a broker host outside the paper allowlist is present:\n",
              file=sys.stderr)
        for line in failures:
            print(f"  {line}", file=sys.stderr)
        print("\nThis project is paper-only by construction. If a new endpoint is genuinely "
              "needed, adding it to the allowlist is a deliberate act and belongs in a commit "
              "of its own.", file=sys.stderr)
        return 1

    print("ok -- every broker host present is on the paper allowlist")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
