#!/usr/bin/env python3
"""Load-bearing factual claims in the documentation, checked against reality.

WHY THIS EXISTS

Two failures of the same kind, one small and one not.

The small one: `STATUS.md` has said 327, 367, 466, 483 C++ tests and 213, 222, 228,
234 Python ones, and has been wrong about at least one of them at the end of most
sessions -- twice inside Session H alone, both times because a number was copied
from a run taken before the last commit. `FREEZE_CONTRACT.md` granted permission to
correct them, which addressed the permission and not the recurrence.

The one that matters: `README.md` said **"no network code, no API clients, no
credentials"** until Session F, when the live execution seam arrived and made it
false. Nobody noticed. It was corrected by hand, once somebody happened to read it.

Those are the same failure. A document states something about the repository, the
repository changes, and the document keeps saying it -- with no mechanism anywhere
that reads both. One CI step covers the class.

WHAT A CLAIM IS, AND WHY EACH ONE IS ANCHORED

A claim is a regex that must match a named file, plus a way of computing what the
truth is. The pattern is deliberately anchored to its surrounding sentence rather
than being a general search, for one important reason:
`docs/FREEZE_CONTRACT.md` distinguishes a *descriptive* statement about the
repository today from a *historical* one about what was true when a passage was
written, and says the second must never be corrected. Four documents say the plugin
ABI is "frozen at v1"; every one of them is correct, because each explains why the
code around it looks the way it does. A checker that grepped for `frozen at v\\d`
would flag all four and would deserve to be switched off.

So `abi_version` matches the row of the "What exists" table and nothing else.

EVERY CLAIM MUST STILL BE FOUND

A claim whose pattern stops matching -- because the sentence was reworded -- is a
claim that silently stops being checked, which is the vacuous-pass law applied to
this script. So a pattern that finds nothing is a failure, not a skip.

    python scripts/check_doc_claims.py --claims cpp_tests,cpp_tests_full,abi_version,guards_wired,tests_offline --test-dir out/build/ci
    python scripts/check_doc_claims.py --claims python_tests
    ... --write to update the numeric ones in place.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORKFLOW = REPO / ".github" / "workflows" / "ci.yml"

# The only types in this repository that can open a socket. A test file that does
# not name one of them cannot reach the network whatever methods it calls, which is
# what makes the scan below precise rather than noisy.
#
# The first draft keyed on the *variable* name instead and flagged
# `broker.submit()` in test_execution.cpp, test_ledger.cpp and
# test_recorded_broker.cpp -- the fill simulator and the replay broker, neither of
# which has a socket. Five false positives on the first run. A guard that cries wolf
# is a guard somebody switches off, so it keys on the type now.
NETWORK_CAPABLE_TYPES = ("LiveBroker", "LiveMarketDataFeed", "HttpClient", "WebSocket")

# Methods that actually reach the network, on one of those types. Calling one in a
# test would make README's "No test touches the network" false.
NETWORK_ENTRY = (
    "start",
    "submit",
    "cancel",
    "cancel_all_open",
    "reconcile",
    "get",
    "post",
    "del",
)

# Calls that look like network entry and provably are not, with the reason and the
# expected number of them. The count is part of the entry so that *adding* a call
# fails even when one was already allowed -- an allowlist keyed only on the method
# name would wave through a second call that did reach the network.
ALLOWED_NETWORK_CALLS = {
    ("tests/test_live_broker.cpp", "submit", 1): (
        "refused by the share-cap rail before the POST; the test asserts "
        "kInvalidOrderId and that submitted stayed 0, which is the evidence it "
        "returned early"
    ),
    ("tests/test_live_broker.cpp", "cancel", 1): (
        "the order is already Filled, so cancel() takes the lost-race branch and "
        "returns false before any HTTP; the test asserts the race counter"
    ),
}


def ctest_total(test_dir: str | None, preset: str | None) -> int:
    command = ["ctest", "-N"]
    if preset:
        command += ["--preset", preset]
    if test_dir:
        command += ["--test-dir", test_dir]
    done = subprocess.run(command, cwd=REPO, capture_output=True, text=True)
    if done.returncode != 0:
        print(done.stdout + done.stderr, file=sys.stderr)
        raise SystemExit(f"ctest -N exited {done.returncode}")
    for line in (done.stdout + done.stderr).splitlines():
        found = re.match(r"^Total Tests:\s*(\d+)\s*$", line.strip())
        if found:
            return int(found.group(1))
    raise SystemExit("ctest -N did not report a total; cannot check the test-count claims")


def pytest_total(target: str = "research/tests") -> int:
    command = [sys.executable, "-m", "pytest", target, "--collect-only", "-q", "-p", "no:randomly"]
    done = subprocess.run(command, cwd=REPO, capture_output=True, text=True)
    if done.returncode != 0:
        print(done.stdout + done.stderr, file=sys.stderr)
        raise SystemExit(f"pytest --collect-only exited {done.returncode}")
    for line in done.stdout.splitlines():
        found = re.match(r"^(\d+)\s+tests?\s+collected\b", line.strip())
        if found:
            return int(found.group(1))
    raise SystemExit("pytest did not report a collected count; cannot check the Python claim")


def abi_version() -> int:
    header = (REPO / "include" / "marketscan" / "abi" / "plugin_abi.h").read_text(encoding="utf-8")
    found = re.search(r"#define\s+MS_ABI_VERSION\s+(\d+)u?\b", header)
    if not found:
        raise SystemExit("MS_ABI_VERSION not found in plugin_abi.h")
    return int(found.group(1))


def network_capable_test_files() -> list[Path]:
    """Test sources that name a type able to open a socket."""
    names = re.compile(r"\b(?:" + "|".join(NETWORK_CAPABLE_TYPES) + r")\b")
    return [p for p in sorted((REPO / "tests").glob("*.cpp")) if names.search(
        p.read_text(encoding="utf-8"))]


def network_calls_in_tests() -> dict[tuple[str, str], int]:
    """Every call to a network-entry method, in the files that could make one."""
    pattern = re.compile(r"(?:->|\.)(" + "|".join(NETWORK_ENTRY) + r")\s*\(")
    counts: dict[tuple[str, str], int] = {}
    for path in network_capable_test_files():
        text = path.read_text(encoding="utf-8")
        for match in pattern.finditer(text):
            key = (path.relative_to(REPO).as_posix(), match.group(1))
            counts[key] = counts.get(key, 0) + 1
    return counts


def workflow_text() -> str:
    return WORKFLOW.read_text(encoding="utf-8")


# --------------------------------------------------------------------------
# The registry
# --------------------------------------------------------------------------
#
# Each numeric claim is (file, pattern with one group, how to compute the truth).
# Each structural claim is a callable returning a list of complaints.


def numeric_claims(args) -> dict:
    return {
        "cpp_tests": {
            "file": "docs/STATUS.md",
            # Both the header line and the "What exists" line say this number.
            "pattern": r"(\d+) C\+\+ tests",
            "truth": lambda: ctest_total(args.test_dir, args.preset),
            "what": "the number of tests ctest registers",
        },
        "cpp_tests_full": {
            "file": "docs/STATUS.md",
            # The `full` preset runs one more; AUDIT.md check 9 explains why.
            "pattern": r"\*\*(\d+)\*\* on `full`",
            "truth": lambda: ctest_total(args.test_dir, args.preset) + 1,
            "what": "the ctest total plus the one extra test the `full` preset runs",
        },
        "python_tests": {
            "file": "docs/STATUS.md",
            "pattern": r"(\d+) Python(?: tests)?[.*]",
            "truth": lambda: pytest_total(),
            "what": "the number of tests pytest collects",
        },
        "abi_version": {
            "file": "docs/STATUS.md",
            # Anchored to the "What exists" row so the four *historical* "frozen at
            # v1" statements elsewhere are untouched. See FREEZE_CONTRACT.md.
            "pattern": r"\*\*Plugin ABI\*\* \| pure C, frozen at v(\d+)",
            "truth": abi_version,
            "what": "MS_ABI_VERSION in include/marketscan/abi/plugin_abi.h",
        },
    }


def check_guards_wired() -> list[str]:
    """README names three enforcement mechanisms. Do they exist, and does CI run them?

    This is the "no network code, no API clients, no credentials" claim in its
    current, narrower form. The claim is no longer that there is no network code --
    there is -- but that specific guards constrain it. A named guard that was
    deleted, or left in the tree but unwired from CI, makes the claim false in
    exactly the way the original went false: quietly.
    """
    complaints: list[str] = []
    readme = (REPO / "README.md").read_text(encoding="utf-8")
    ci = workflow_text()

    if "enforced three ways" not in readme:
        complaints.append(
            "README.md no longer says the paper allowlist is 'enforced three ways'. If the "
            "enforcement changed, update this check; if the wording changed, re-anchor it."
        )

    for script, needs in (
        ("scripts/check_no_live_endpoints.py", ("--binaries",)),
        ("scripts/check_no_secrets.py", ()),
    ):
        if not (REPO / script).is_file():
            complaints.append(f"{script} is named by README.md and does not exist.")
            continue
        if script.split("/")[-1] not in ci:
            complaints.append(
                f"{script} exists but .github/workflows/ci.yml never runs it, so the "
                "claim it backs is unenforced."
            )
        for flag in needs:
            if flag not in ci:
                complaints.append(
                    f"ci.yml runs {script} but never with {flag}, so only one of the two "
                    "scans README.md describes actually happens."
                )

    # The third way: a runtime refusal, not a scan.
    http = (REPO / "src" / "marketscan" / "live" / "http.cpp").read_text(encoding="utf-8")
    if "is_allowed_host" not in http or "throw" not in http:
        complaints.append(
            "src/marketscan/live/http.cpp no longer refuses a non-allowlisted host at "
            "runtime, which is the third of the three ways README.md claims."
        )

    return complaints


def check_tests_offline() -> list[str]:
    """README: "No test touches the network." Is that still true?

    Checked by finding every call in the test sources to a method that reaches the
    network, and requiring each to be allowlisted with a reason and a count. Two are:
    a submit refused by a rail and a cancel on an order that is already over, both of
    which return before any HTTP and both of whose tests assert the early return.
    """
    complaints: list[str] = []
    readme = (REPO / "README.md").read_text(encoding="utf-8")

    if "No test touches the network" not in readme:
        complaints.append(
            "README.md no longer claims 'No test touches the network'. Re-anchor this "
            "check or remove it; do not leave it matching nothing."
        )

    found = network_calls_in_tests()
    allowed = {(f, m): (n, why) for (f, m, n), why in ALLOWED_NETWORK_CALLS.items()}

    for (path, method), count in sorted(found.items()):
        entry = allowed.get((path, method))
        if entry is None:
            complaints.append(
                f"{path} calls {method}() {count}x on a live-layer object, and nothing says "
                "why that does not reach the network. If it returns before any HTTP, add it "
                "to ALLOWED_NETWORK_CALLS with the reason; if it does not, the test is "
                "reaching a venue and BUILD_SPEC_2.md section 2 forbids that."
            )
            continue
        expected, _ = entry
        if count != expected:
            complaints.append(
                f"{path} calls {method}() {count}x; {expected} were allowlisted. A new call "
                "is not covered by the old reason -- check it and update the count."
            )

    for (path, method, expected), why in sorted(ALLOWED_NETWORK_CALLS.items()):
        if (path, method) not in found:
            complaints.append(
                f"the allowlist entry for {path}::{method} ({expected}x) no longer matches "
                f"any call. Delete it. Reason on file: {why}"
            )

    return complaints


STRUCTURAL_CLAIMS = {
    "guards_wired": (check_guards_wired, "the guards README.md names exist and CI runs them"),
    "tests_offline": (check_tests_offline, "no test reaches the network"),
}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--claims",
        default="",
        help="comma-separated claim ids to check; default is all of them",
    )
    parser.add_argument("--test-dir", help="build directory, for the ctest-count claims")
    parser.add_argument("--preset", help="ctest preset, for the ctest-count claims")
    parser.add_argument("--write", action="store_true", help="update numeric claims in place")
    parser.add_argument("--list", action="store_true", help="print every claim id and exit")
    args = parser.parse_args(argv)

    numeric = numeric_claims(args)
    every = sorted(set(numeric) | set(STRUCTURAL_CLAIMS))

    if args.list:
        for claim in every:
            what = numeric[claim]["what"] if claim in numeric else STRUCTURAL_CLAIMS[claim][1]
            print(f"{claim}\t{what}")
        return 0

    # Split on commas *and* whitespace: PowerShell turns `a,b,c` into an array and
    # hands the script one space-joined argument, so a comma-only split rejects
    # exactly the form the CI workflow would naturally use.
    selected = [c for c in re.split(r"[,\s]+", args.claims) if c] or every
    unknown = [c for c in selected if c not in every]
    if unknown:
        raise SystemExit(f"unknown claim(s): {unknown}. Known: {every}")

    failures: list[str] = []
    checked = 0

    for claim in selected:
        if claim in numeric:
            spec = numeric[claim]
            path = REPO / spec["file"]
            text = path.read_text(encoding="utf-8")
            matches = list(re.finditer(spec["pattern"], text))

            # A pattern that matches nothing is a claim that stopped being checked.
            if not matches:
                failures.append(
                    f"[{claim}] the pattern {spec['pattern']!r} matches nothing in "
                    f"{spec['file']}. The sentence was reworded, so this claim is no longer "
                    "being checked at all -- re-anchor it rather than leaving it silent."
                )
                continue

            truth = spec["truth"]()
            checked += 1
            wrong = [m for m in matches if int(m.group(1)) != truth]
            if not wrong:
                continue

            if args.write:
                for m in reversed(wrong):
                    start, end = m.span(1)
                    text = text[:start] + str(truth) + text[end:]
                path.write_text(text, encoding="utf-8", newline="\n")
                print(f"[{claim}] corrected {len(wrong)} occurrence(s) in {spec['file']} "
                      f"to {truth}")
            else:
                stale = ", ".join(sorted({m.group(1) for m in wrong}))
                failures.append(
                    f"[{claim}] {spec['file']} says {stale}; {spec['what']} is {truth}. "
                    "Run with --write."
                )
        else:
            check, _ = STRUCTURAL_CLAIMS[claim]
            checked += 1
            for complaint in check():
                failures.append(f"[{claim}] {complaint}")

    if checked == 0:
        print("no claims were checked, so nothing was verified.", file=sys.stderr)
        return 1

    if failures:
        print(f"\n{len(failures)} documentation claim(s) do not match reality:\n",
              file=sys.stderr)
        for failure in failures:
            print(f"  {failure}\n", file=sys.stderr)
        print(
            "A document that states something untrue about this repository is a bug even "
            "when nothing else is wrong. See docs/FREEZE_CONTRACT.md for which claims are "
            "corrected and which are left alone.",
            file=sys.stderr,
        )
        return 1

    print(f"doc claims: {checked} of {len(every)} verified ({', '.join(selected)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
