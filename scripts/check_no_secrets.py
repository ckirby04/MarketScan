#!/usr/bin/env python3
"""Fails if anything resembling a broker credential is in the tree.

A gitignore is a convenience that one `git add -f`, one misconfigured editor, or
one helpful tool defeats. This is the check that runs anyway.

WHAT IT LOOKS FOR

Alpaca key ids are twenty upper-case alphanumerics with a two-letter prefix;
secrets are forty characters of base64 alphabet. Both shapes are common enough in
ordinary text -- a commit hash, a base64 blob, a long identifier -- that matching
on shape alone would cry wolf until somebody turned it off, which is worse than
not having it.

So a match needs the shape **and** a credential-ish word nearby: the variable
names this project reads, or the words key, secret, token or credential on the
same line. That is a deliberate trade. It will miss a key pasted into a file with
no context at all, and it will not produce the false positives that get a check
disabled.

    python scripts/check_no_secrets.py
    python scripts/check_no_secrets.py --staged      # what is about to be committed
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

KEY_SHAPE = re.compile(r"\b[A-Z]{2}[A-Z0-9]{18}\b")
SECRET_SHAPE = re.compile(r"\b[A-Za-z0-9/+]{40}\b")

CONTEXT = re.compile(r"APCA_API|ALPACA|\bapi[_ -]?key\b|\bsecret\b|\btoken\b|\bcredential",
                     re.IGNORECASE)

SKIP_DIRS = {"out", "build", ".git", "vcpkg_installed", "__pycache__", ".vs"}
SKIP_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".pdf", ".zip", ".dll", ".exe", ".lib",
                 ".obj", ".pdb", ".msb"}

SELF = Path(__file__).name


def suspicious(text: str) -> list[tuple[int, str]]:
    hits: list[tuple[int, str]] = []
    for number, line in enumerate(text.splitlines(), start=1):
        if not CONTEXT.search(line):
            continue
        for pattern, what in ((KEY_SHAPE, "key id"), (SECRET_SHAPE, "secret")):
            match = pattern.search(line)
            if match:
                # The value is never printed. A check that echoes the secret it
                # found has published it to a CI log.
                hits.append((number, f"something shaped like a {what} beside a credential word"))
                break
    return hits


def tracked_files() -> list[Path]:
    out = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True, text=True,
                         check=False)
    return [REPO / line for line in out.stdout.splitlines() if line]


def staged_files() -> list[Path]:
    out = subprocess.run(["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"],
                         cwd=REPO, capture_output=True, text=True, check=False)
    return [REPO / line for line in out.stdout.splitlines() if line]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--staged", action="store_true",
                        help="check only what is staged, for a pre-commit hook")
    args = parser.parse_args(argv)

    paths = staged_files() if args.staged else tracked_files()
    failures: list[str] = []
    scanned = 0

    for path in paths:
        if not path.is_file():
            continue
        if SKIP_DIRS & set(path.parts) or path.suffix.lower() in SKIP_SUFFIXES:
            continue
        if path.name == SELF:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        scanned += 1
        for number, why in suspicious(text):
            failures.append(f"{path.relative_to(REPO)}:{number}: {why}")

    print(f"scanned {scanned} tracked files for credential shapes")

    if failures:
        print("\nFAILED -- something in the tree looks like a credential:\n", file=sys.stderr)
        for line in failures:
            print(f"  {line}", file=sys.stderr)
        print("\nCredentials come from the environment and are never committed. See "
              "docs/DATA_POLICY.md. If this is a false positive, the line probably wants "
              "rewording rather than the check weakening.", file=sys.stderr)
        return 1

    print("ok -- nothing resembling a credential is tracked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
