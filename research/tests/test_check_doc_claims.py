"""The doc-claims gate, and the one question it cannot answer about itself.

`scripts/check_doc_claims.py` validates documented facts against the repository.
Its claims are checked across two CI jobs, because no single job has both a C++
build tree and pytest's dependencies. That split creates a hole the script cannot
see from inside: **a claim checked by neither job still reports success, because
nothing ever asks it.**

So the closing assertion lives here, and it reads the workflow. Every claim in the
registry must be named by at least one CI invocation. A claim added tomorrow and
wired into nothing fails this test rather than sitting in the registry looking
enforced.

That is the same move as everything else in this repository: the mechanism whose
absence is invisible gets an assertion pointed at it.
"""

from __future__ import annotations

import importlib.util
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
WORKFLOW = REPO / ".github" / "workflows" / "ci.yml"


def _load():
    spec = importlib.util.spec_from_file_location(
        "check_doc_claims", REPO / "scripts" / "check_doc_claims.py"
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


claims = _load()


class _Args:
    test_dir = None
    preset = None


def every_claim() -> set[str]:
    return set(claims.numeric_claims(_Args())) | set(claims.STRUCTURAL_CLAIMS)


def claims_named_in_ci() -> set[str]:
    text = WORKFLOW.read_text(encoding="utf-8")
    named: set[str] = set()
    for match in re.finditer(r"--claims\s+\"?([\w,\s]+?)\"?\s*(?:\n|$|--)", text):
        for claim in re.split(r"[,\s]+", match.group(1)):
            if claim:
                named.add(claim)
    return named


def test_the_registry_is_not_empty():
    """A registry of nothing would satisfy every assertion below."""
    assert len(every_claim()) >= 5, f"only {every_claim()} registered; that is not the registry"


def test_every_claim_is_checked_by_at_least_one_ci_job():
    registered = every_claim()
    named = claims_named_in_ci()

    assert named, (
        "no --claims invocation was found in .github/workflows/ci.yml. Either the "
        "workflow stopped running check_doc_claims.py, or this test's pattern no "
        "longer matches how it is invoked -- and a test that finds nothing to check "
        "is the failure this whole file is about."
    )

    unchecked = sorted(registered - named)
    assert not unchecked, (
        f"these claims are in the registry and no CI job checks them: {unchecked}\n\n"
        "A claim nobody asks about reports success forever. Add it to one of the "
        "--claims lists in .github/workflows/ci.yml -- the windows job for anything "
        "needing a build tree, the research job for anything needing pytest."
    )


def test_ci_does_not_name_a_claim_that_does_not_exist():
    """The other direction: a renamed claim leaves a dangling invocation.

    The script already rejects an unknown claim id at runtime, so CI would go red --
    but it would go red in a build job with a build log, rather than here with an
    explanation. Failing fast and locally is worth the four lines.
    """
    registered = every_claim()
    dangling = sorted(claims_named_in_ci() - registered)
    assert not dangling, (
        f"ci.yml asks for claims that are not in the registry: {dangling}. "
        "They were renamed or removed; update the workflow."
    )


def test_the_two_jobs_between_them_cover_the_registry_exactly_once_each():
    """Not a correctness requirement -- a claim checked twice is harmless -- but
    worth knowing about, because duplication across jobs usually means somebody
    moved a claim and did not delete the old invocation."""
    text = WORKFLOW.read_text(encoding="utf-8")
    counts: dict[str, int] = {}
    for match in re.finditer(r"--claims\s+\"?([\w,\s]+?)\"?\s*(?:\n|$|--)", text):
        for claim in re.split(r"[,\s]+", match.group(1)):
            if claim:
                counts[claim] = counts.get(claim, 0) + 1
    repeated = sorted(c for c, n in counts.items() if n > 1)
    assert not repeated, f"these claims are checked by more than one job: {repeated}"


def test_the_network_allowlist_entries_all_carry_a_reason():
    """An allowlist entry without a reason is an entry nobody can review."""
    assert claims.ALLOWED_NETWORK_CALLS, "the allowlist is empty; that is suspicious, not clean"
    for key, reason in claims.ALLOWED_NETWORK_CALLS.items():
        path, method, count = key
        assert isinstance(count, int) and count > 0, f"{key} has no expected count"
        assert len(reason) > 40, (
            f"{path}::{method} is allowlisted with the reason {reason!r}, which is too "
            "short to review. Say why the call does not reach the network."
        )
        assert (REPO / path).is_file(), f"{path} is allowlisted and does not exist"


def test_only_network_capable_files_are_scanned():
    """The scan keys on type, not on variable name.

    Its first draft keyed on names like `broker` and flagged the fill simulator and
    the replay broker -- five false positives, in files with no socket anywhere near
    them. This holds the fix: only files naming a network-capable type are scanned at
    all.
    """
    scanned = {p.name for p in claims.network_capable_test_files()}
    assert scanned, "no test file names a network-capable type; the scan covers nothing"
    assert "test_live_broker.cpp" in scanned
    # The ones that tripped the first draft must stay out of scope.
    for innocent in ("test_execution.cpp", "test_ledger.cpp", "test_recorded_broker.cpp"):
        assert innocent not in scanned, (
            f"{innocent} is being scanned for network calls. It uses the simulator or the "
            "replay broker, neither of which has a socket -- if it now names a "
            "network-capable type, that is the thing to look at."
        )
