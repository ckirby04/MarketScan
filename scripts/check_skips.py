#!/usr/bin/env python3
"""Runs ctest, enumerates every skipped test, and fails on any not allowlisted.

WHY THIS EXISTS

"351 passed, 1 skipped" reads like a pass. It is not, and this repository has the
scar: one skipped test persisted across four reported counts -- 334, 343, 346,
351 -- was read each time as cosmetic, and was the only test that would have
revealed the identity classifier had never been validated against a real price
series. A silent failure wearing a friendly face.

So: every skip is named in the job summary with its reason, and a skip that is
not in `tests/allowed_skips.txt` fails the build. Allowlisting is a deliberate
act with a written justification, not a default.

WHY IT PARSES THE CONSOLE RATHER THAN THE JUNIT XML

`ctest --output-junit` does not emit skipped tests at all. A run with 351 passes
and one skip produces a file containing 351 testcases, every one of them
`status="run"`, and a `skipped="0"` attribute on the suite. Trusting it would
reproduce the exact failure this script exists to prevent, so the skips are taken
from the block ctest prints at the end:

    The following tests did not run:
            122 - the classifier agrees with the cases we are sure of (Skipped)

    python scripts/check_skips.py --test-dir out/build/ci -j 3
    python scripts/check_skips.py --preset ci
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ALLOWLIST = REPO / "tests" / "allowed_skips.txt"

# "        122 - some test name (Skipped)" -- the number and the reason in
# brackets both vary, and the name is what identifies the test.
SKIP_LINE = re.compile(r"^\s*\d+\s*-\s*(?P<name>.+?)\s*\((?P<status>[A-Za-z ]+)\)\s*$")

NOT_RUN_HEADER = "The following tests did not run:"


def load_allowlist(path: Path) -> dict[str, str]:
    """`name :: reason` per line. Missing file is an empty allowlist, not an error."""
    allowed: dict[str, str] = {}
    if not path.exists():
        return allowed
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        name, separator, reason = line.partition("::")
        if not separator:
            raise SystemExit(f"{path}: line is missing '::' -- {raw!r}")
        if not reason.strip():
            raise SystemExit(f"{path}: '{name.strip()}' has no reason. That is the point of "
                             f"the file; an entry without one is a silent skip with extra steps.")
        allowed[name.strip()] = reason.strip()
    return allowed


def parse_skips(output: str) -> list[tuple[str, str]]:
    """(name, status) for every test ctest reported as not having run."""
    skipped: list[tuple[str, str]] = []
    in_block = False
    for line in output.splitlines():
        if NOT_RUN_HEADER in line:
            in_block = True
            continue
        if not in_block:
            continue
        match = SKIP_LINE.match(line)
        if match:
            skipped.append((match.group("name"), match.group("status")))
            continue
        # The block is contiguous; a non-matching, non-blank line ends it.
        if line.strip():
            in_block = False
    return skipped


def write_summary(lines: list[str]) -> None:
    """Appends to the GitHub step summary when running in Actions, stdout otherwise."""
    text = "\n".join(lines) + "\n"
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as handle:
            handle.write(text)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--preset", help="ctest preset to run")
    parser.add_argument("--test-dir", help="build directory to run ctest in")
    parser.add_argument("--allowlist", type=Path, default=ALLOWLIST)
    parser.add_argument("rest", nargs=argparse.REMAINDER,
                        help="further arguments passed to ctest, after --")
    # parse_known_args, not parse_args: ctest flags like -j 3 are meant to reach
    # ctest, and REMAINDER only collects them when a bare -- separates them.
    # Requiring the separator is a footgun for the one caller this has.
    args, extra = parser.parse_known_args(argv)
    args.rest = list(args.rest) + list(extra)

    command = ["ctest"]
    if args.preset:
        command += ["--preset", args.preset]
    if args.test_dir:
        command += ["--test-dir", args.test_dir]
    command += ["--output-on-failure"]
    command += [a for a in args.rest if a != "--"]

    print("$ " + " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=REPO, capture_output=True, text=True)
    output = completed.stdout + completed.stderr
    print(output, flush=True)

    allowed = load_allowlist(args.allowlist)
    skipped = parse_skips(output)

    lines = ["## Skipped tests", ""]
    if not skipped:
        lines.append("None. Every registered test ran.")
    else:
        lines.append(f"{len(skipped)} test(s) did not run. A skip is not a pass.")
        lines.append("")
        for name, status in skipped:
            reason = allowed.get(name)
            mark = "allowlisted" if reason else "**NOT ALLOWLISTED**"
            lines.append(f"- `{name}` ({status.lower()}) -- {mark}")
            if reason:
                lines.append(f"  - {reason}")

    stale = sorted(set(allowed) - {name for name, _ in skipped})
    if stale:
        lines += ["", "### Allowlist entries that did not skip", ""]
        for name in stale:
            lines.append(f"- `{name}` ran. If it now runs everywhere, delete its entry.")

    unexpected = [name for name, _ in skipped if name not in allowed]
    if unexpected:
        lines += ["", "### Unexpected skips -- failing the job", ""]
        for name in unexpected:
            lines.append(f"- `{name}`")
        lines.append("")
        lines.append("Add it to `tests/allowed_skips.txt` with a reason, or make it run. "
                     "Do not leave it silent.")

    write_summary(lines)

    if completed.returncode != 0:
        print(f"ctest exited {completed.returncode}", file=sys.stderr)
        return completed.returncode
    if unexpected:
        print(f"{len(unexpected)} unexpected skip(s); see the summary above.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
