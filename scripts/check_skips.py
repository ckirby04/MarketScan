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

AND IT STATES ITS OWN DENOMINATOR

This script used to print "None. Every registered test ran." whenever the skip
list came back empty -- which is a claim it could not support, because an empty
skip list is also exactly what a run of zero tests produces. The *gate* held even
then: ctest exits non-zero when no test matches, which was measured rather than
assumed, and that exit code is returned. But `write_summary` runs first, so the
summary asserted something it did not know while the job failed around it, and the
summary is what a person reads.

Now the test total is parsed from ctest's own output, printed, and required: a run
that does not report a total fails, and so does one below `--min-tests`. A report
that cannot name its denominator has nothing to say -- see the vacuous-pass law in
docs/AUDIT.md.

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

# The only place ctest states how many tests it knew about -- and it states it in
# more than one shape, which cost a red CI run to find out:
#
#   100% tests passed out of 483                      (some versions, nothing failed)
#   100% tests passed, 0 tests failed out of 483      (others, nothing failed)
#   97% tests passed, 3 tests failed out of 483       (something failed)
#
# The first draft of this required the "N tests failed" clause. It matched the
# local ctest and not the one on the CI runner, so a perfectly healthy run reported
# "ctest reported no test total" and the gate failed the build. The failure clause
# is therefore optional, and all three forms are in the tests.
TOTAL_LINE = re.compile(
    r"tests?\s+passed(?:\s*,\s*\d+\s+tests?\s+failed)?\s+out\s+of\s+(?P<total>\d+)\b"
)

# A floor, not a tracker.
#
# It exists to catch a run that tested almost nothing -- the wrong --test-dir, a
# preset whose test set collapsed, a -R filter matching a handful -- and not to
# record the exact count, which changes every time a case is added and would make
# this a chore rather than a guard. Raise it when the suite grows a lot; it should
# stay comfortably below the real total.
DEFAULT_MIN_TESTS = 400


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


def parse_total(output: str) -> int | None:
    """How many tests ctest knew about, or None when it did not say.

    None is a failure rather than a shrug: a summary that cannot name its own
    denominator has nothing to report.
    """
    total: int | None = None
    for line in output.splitlines():
        match = TOTAL_LINE.search(line)
        if match:
            total = int(match.group("total"))
    return total


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
    parser.add_argument(
        "--min-tests",
        type=int,
        default=DEFAULT_MIN_TESTS,
        help="fail if ctest registered fewer tests than this (default: %(default)s)",
    )
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
    total = parse_total(output)

    # How many tests there were, before anything is said about how many ran.
    #
    # This block used to say "None. Every registered test ran." on the strength of
    # an empty skip list -- a claim it could not support, because an empty skip
    # list is also what a run of zero tests produces. The gate held even then
    # (ctest exits non-zero when no test matches, measured), but the job summary
    # asserted something it did not know, and the summary is what a person reads.
    # See docs/AUDIT.md, "Where the law is not yet applied".
    lines = ["## Skipped tests", ""]
    if total is None:
        lines += [
            "**ctest did not report a test total, so this summary cannot say how many "
            "tests ran.** Nothing below should be read as a pass.",
            "",
        ]
    elif total < args.min_tests:
        lines += [
            f"**Only {total} test{'' if total == 1 else 's'} {'was' if total == 1 else 'were'} "
            f"registered, below the floor of {args.min_tests}.** A run this small has not "
            "exercised the suite, whatever it reports about skips.",
            "",
        ]
    else:
        lines += [f"{total} tests registered, {total - len(skipped)} ran.", ""]

    if not skipped:
        lines.append("None skipped." if total is not None else "No skips were reported.")
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
    if total is None:
        print(
            "ctest reported no test total, so this run cannot be confirmed to have tested "
            "anything.",
            file=sys.stderr,
        )
        return 1
    if total < args.min_tests:
        print(
            f"only {total} test{'' if total == 1 else 's'} registered, below the --min-tests "
            f"floor of {args.min_tests}. A run this small has not exercised the suite.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
