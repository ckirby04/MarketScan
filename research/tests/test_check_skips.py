"""The skip gate, tested — because it is the thing that catches silent failures.

`scripts/check_skips.py` is not part of the research layer and lives here only
because this is where the Python test suite is. It earns a test for the same
reason it exists: one skipped test survived four reported counts in this
repository, and the gate that now catches that must not itself fail quietly.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent.parent
ALLOWLIST = REPO / "tests" / "allowed_skips.txt"


def _load():
    spec = importlib.util.spec_from_file_location(
        "check_skips", REPO / "scripts" / "check_skips.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


check_skips = _load()


# ---------------------------------------------------------------------------
# Parsing what ctest actually prints
# ---------------------------------------------------------------------------

CTEST_TAIL = """\
100% tests passed, 0 tests failed out of 351

Label Time Summary:
unit    =  54.53 sec*proc (351 tests)

Total Test time (real) =  44.09 sec

The following tests did not run:
\t122 - the classifier agrees with the cases we are sure of (Skipped)
"""


def test_a_skip_is_found_in_the_block_ctest_prints():
    found = check_skips.parse_skips(CTEST_TAIL)
    assert found == [("the classifier agrees with the cases we are sure of", "Skipped")]


def test_a_clean_run_reports_no_skips():
    clean = CTEST_TAIL.split("The following tests did not run:")[0]
    assert check_skips.parse_skips(clean) == []


def test_several_skips_are_all_found():
    text = ("The following tests did not run:\n"
            "\t3 - one (Skipped)\n"
            "\t9 - two (Not Run)\n"
            "\t11 - a name with (parentheses) in it (Skipped)\n")
    assert check_skips.parse_skips(text) == [
        ("one", "Skipped"),
        ("two", "Not Run"),
        ("a name with (parentheses) in it", "Skipped"),
    ]


def test_the_block_ends_at_the_first_unrelated_line():
    """ctest prints other things after the block on some runs; they are not skips."""
    text = ("The following tests did not run:\n"
            "\t3 - one (Skipped)\n"
            "\n"
            "Errors while running CTest\n"
            "\t99 - something else entirely (Failed)\n")
    assert check_skips.parse_skips(text) == [("one", "Skipped")]


# ---------------------------------------------------------------------------
# The allowlist
# ---------------------------------------------------------------------------


def test_the_committed_allowlist_parses_and_every_entry_has_a_reason():
    allowed = check_skips.load_allowlist(ALLOWLIST)
    assert allowed, "the allowlist is empty; the current skip would fail the job"
    assert len(allowed) == 1, "the allowlist grew or shrank; that is a deliberate act"
    for name, reason in allowed.items():
        assert name.strip() == name
        # Long enough to be an argument rather than a shrug. "needs data" is not
        # a justification anybody can evaluate.
        assert len(reason) > 80, f"{name}: the reason is too short to be one"


def test_the_current_skip_is_the_one_that_is_allowlisted():
    allowed = check_skips.load_allowlist(ALLOWLIST)
    assert "the classifier agrees with the cases we are sure of" in allowed
    reason = allowed["the classifier agrees with the cases we are sure of"]
    # The reason has to say what would change it, not merely that it is broken.
    assert "CRSP" in reason
    assert "DATA_POLICY" in reason


def test_an_entry_without_a_reason_is_refused(tmp_path):
    path = tmp_path / "allowed.txt"
    path.write_text("some test ::   \n", encoding="utf-8")
    with pytest.raises(SystemExit, match="no reason"):
        check_skips.load_allowlist(path)


def test_a_line_without_the_separator_is_refused(tmp_path):
    path = tmp_path / "allowed.txt"
    path.write_text("some test with no separator\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="missing"):
        check_skips.load_allowlist(path)


def test_comments_and_blank_lines_are_ignored(tmp_path):
    path = tmp_path / "allowed.txt"
    path.write_text("# a comment\n\n  \nreal :: because of a good reason\n", encoding="utf-8")
    assert check_skips.load_allowlist(path) == {"real": "because of a good reason"}


def test_a_missing_allowlist_is_empty_rather_than_an_error(tmp_path):
    """An empty allowlist makes every skip unexpected, which is the safe direction."""
    assert check_skips.load_allowlist(tmp_path / "nope.txt") == {}
