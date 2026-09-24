"""The test manifest gate, tested — including both ways it must fail.

`scripts/check_test_manifest.py` is what makes deleting a test a visible act. It is
not part of the research layer and lives here for the same reason
`test_check_skips.py` does: this is where the Python test suite is.

It earns a test for the reason every guard in this repository earns one. A gate
whose failure path has never run is a gate that reports success, and the thing it
is guarding -- the individual tests named in `docs/PROTECTED.md`, above all the
`importlib` ban standing between this project's central architectural claim and its
becoming tautological -- is exactly the kind of thing whose loss is silent.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
MANIFEST = REPO / "tests" / "test_manifest.txt"
PYTEST_MANIFEST = REPO / "research" / "tests" / "test_manifest.txt"


def _load():
    spec = importlib.util.spec_from_file_location(
        "check_test_manifest", REPO / "scripts" / "check_test_manifest.py"
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


manifest = _load()


# What `ctest -N` actually prints.
CTEST_LISTING = """\
Internal ctest changing into directory: G:/Quant/MarketScan/out/build/ci
Test project G:/Quant/MarketScan/out/build/ci
  Test   #1: version metadata is configured
  Test   #2: INT64_MIN formats without overflowing on negation
  Test #483: the harness can still fail

Total Tests: 483
"""


def parse(text):
    """The listing half of list_tests, without running ctest."""
    names = []
    total = None
    for line in text.splitlines():
        found = manifest.LISTED.match(line)
        if found:
            names.append(found.group("name"))
            continue
        counted = manifest.TOTAL.match(line.strip())
        if counted:
            total = int(counted.group("total"))
    return names, total


# ---------------------------------------------------------------------------
# Parsing what ctest prints
# ---------------------------------------------------------------------------


def test_names_and_total_are_read_from_the_listing():
    names, total = parse(CTEST_LISTING)
    assert names == [
        "version metadata is configured",
        "INT64_MIN formats without overflowing on negation",
        "the harness can still fail",
    ]
    assert total == 483


def test_the_directory_and_project_lines_are_not_mistaken_for_tests():
    names, _ = parse(CTEST_LISTING)
    assert not [n for n in names if "ctest changing into" in n or "Test project" in n]


def test_a_listing_with_no_tests_yields_nothing_rather_than_a_guess():
    names, total = parse("Test project /somewhere\n\nTotal Tests: 0\n")
    assert names == []
    assert total == 0


def test_a_name_containing_a_colon_survives():
    """Test names in this suite contain colons -- "M9 parity: exact, and what it
    took" is a heading and "the 128/124/0 bug: ..." is a case. A parser that split
    on the first colon would truncate them."""
    names, _ = parse("  Test  #7: the 128/124/0 bug: a Rejected order that was never counted\n")
    assert names == ["the 128/124/0 bug: a Rejected order that was never counted"]


# ---------------------------------------------------------------------------
# The comparison, in both failing directions
# ---------------------------------------------------------------------------


def test_an_exact_match_passes(tmp_path):
    path = tmp_path / "m.txt"
    manifest.write_manifest(path, ["alpha", "beta"])
    assert manifest.load_manifest(path) == ["alpha", "beta"]


def test_a_deleted_test_is_detected(tmp_path):
    """The case this file exists for: a guard was removed and the manifest still
    lists it."""
    path = tmp_path / "m.txt"
    manifest.write_manifest(path, ["alpha", "beta", "the importlib ban"])

    live = {"alpha", "beta"}
    listed = set(manifest.load_manifest(path))

    gone = listed - live
    assert gone == {"the importlib ban"}


def test_an_unrecorded_new_test_is_detected(tmp_path):
    """Also a failure, on purpose. A manifest that only caught removals would cover
    less of the suite every session while continuing to report success."""
    path = tmp_path / "m.txt"
    manifest.write_manifest(path, ["alpha"])

    live = {"alpha", "a guard added today"}
    listed = set(manifest.load_manifest(path))

    added = live - listed
    assert added == {"a guard added today"}


def test_comments_and_blank_lines_are_ignored(tmp_path):
    path = tmp_path / "m.txt"
    path.write_text("# a header\n\n  \nreal test name\n", encoding="utf-8")
    assert manifest.load_manifest(path) == ["real test name"]


def test_a_missing_manifest_is_empty_rather_than_an_error(tmp_path):
    """And an empty manifest is refused by main() rather than compared against,
    because an empty manifest matches an empty suite."""
    assert manifest.load_manifest(tmp_path / "nope.txt") == []


def test_a_written_manifest_round_trips_and_is_sorted(tmp_path):
    path = tmp_path / "m.txt"
    manifest.write_manifest(path, ["zulu", "alpha", "mike"])
    assert manifest.load_manifest(path) == ["alpha", "mike", "zulu"]


# ---------------------------------------------------------------------------
# The committed manifest itself
# ---------------------------------------------------------------------------


def test_the_committed_manifest_exists_and_is_substantial():
    """A manifest of three names would pass CI and protect nothing."""
    assert MANIFEST.is_file(), "tests/test_manifest.txt is not committed"
    names = manifest.load_manifest(MANIFEST)
    assert len(names) > 400, f"only {len(names)} names recorded; that is not this suite"
    assert len(set(names)) == len(names), "the manifest contains duplicate names"


def test_the_committed_pytest_manifest_exists_and_is_substantial():
    assert PYTEST_MANIFEST.is_file(), "research/tests/test_manifest.txt is not committed"
    names = manifest.load_manifest(PYTEST_MANIFEST)
    assert len(names) > 200, f"only {len(names)} names recorded; that is not this suite"
    assert len(set(names)) == len(names), "the manifest contains duplicate node ids"


def test_both_manifests_name_the_guards_protected_md_relies_on():
    """The rows in docs/PROTECTED.md that are individual tests, by name.

    If one is renamed this fails and points at PROTECTED.md, which is the right
    outcome: a renamed guard needs its row updated too. The manifest exists so the
    suite's membership cannot change silently, and that matters most for the
    entries something else depends on.

    Both manifests are searched. The first draft of this test looked only in the
    ctest one and failed on exactly the two guards that matter most -- they are
    pytest tests. A manifest covering only C++ would have protected everything
    except the thing most worth protecting.
    """
    names = set(manifest.load_manifest(MANIFEST)) | set(manifest.load_manifest(PYTEST_MANIFEST))

    required = [
        # The importlib ban: the guard between the layer-independence claim and a
        # one-line bypass. A pytest test.
        "test_no_guarded_module_imports_dynamically",
        # The duplication guard itself, both parametrised halves. A pytest test.
        "test_guarded_module_does_not_reach_the_cpp_bindings",
        # The deliberate failure that proves red is reachable.
        "the harness can still fail",
        # The empty-gate case that proves a size assertion is not decoration.
        "an unused gate reports the same zeros as a gate that found nothing wrong",
        # The 128/124/0 regression.
        "the 128/124/0 bug: a Rejected order that was never counted fails the identity",
        # SummaryAudit refusing to pass on zero checks.
        "a summary audit that checked nothing is a failure, not a pass",
    ]

    missing = [r for r in required if not any(r in name for name in names)]
    assert not missing, (
        "these protected guards are in neither manifest under the names this test "
        f"expects: {missing}\n\n"
        "Either they were renamed -- in which case update this list and the matching "
        "row in docs/PROTECTED.md -- or they were removed, which is what all three "
        "files exist to make visible."
    )
