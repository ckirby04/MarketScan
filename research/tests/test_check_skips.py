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


# ---------------------------------------------------------------------------
# The denominator
#
# This script used to print "None. Every registered test ran." off an empty skip
# list -- which is also what a run of zero tests produces. The gate held anyway,
# because ctest exits non-zero when nothing matches, but the summary asserted
# something it could not know and the summary is what a person reads.
# ---------------------------------------------------------------------------


def test_the_test_total_is_read_from_what_ctest_prints():
    assert check_skips.parse_total(CTEST_TAIL) == 351


# ctest does not print one summary line. It prints at least three, and the first
# version of parse_total matched only the one the local install happened to emit --
# so the tests passed, CI went red on a perfectly healthy run, and the gate this
# script exists to provide failed the build instead of guarding it.
#
# Every form goes in the fixture now. A parser tested against one input shape is a
# parser tested against the shape you happened to see.
TOTAL_LINES = [
    ("100% tests passed out of 483", 483),
    ("100% tests passed, 0 tests failed out of 483", 483),
    ("97% tests passed, 3 tests failed out of 483", 483),
    ("0% tests passed, 1 tests failed out of 1", 1),
    ("50% tests passed, 1 test failed out of 2", 2),
]


@pytest.mark.parametrize("line,expected", TOTAL_LINES)
def test_every_shape_of_ctest_summary_line_yields_its_total(line, expected):
    assert check_skips.parse_total(line) == expected


def test_the_ci_runner_format_is_covered():
    """Named separately because this exact string is what went red.

    The CI ctest omits the failure clause entirely when nothing failed. The local
    one includes it with a zero.
    """
    assert check_skips.parse_total("100% tests passed out of 483\n") == 483


def test_a_run_of_zero_tests_does_not_look_like_a_clean_run():
    """The shape the old summary could not tell apart from success.

    No skips reported, because nothing ran. The skip list is empty and the total
    is zero, and it is the total that distinguishes this from a healthy run.
    """
    nothing = "No tests were found!!!\n"
    assert check_skips.parse_skips(nothing) == []
    assert check_skips.parse_total(nothing) is None


def test_a_missing_total_is_none_rather_than_a_guess():
    assert check_skips.parse_total("some unrelated output\n") is None
    assert check_skips.parse_total("") is None


def test_the_last_total_wins_when_ctest_prints_more_than_one():
    """Two runs in one log: the later line is the one that describes this run."""
    text = "100% tests passed, 0 tests failed out of 12\n" + CTEST_TAIL
    assert check_skips.parse_total(text) == 351


def test_a_failing_run_still_reports_its_total():
    """The total must be readable when tests fail, or the floor stops applying
    exactly when the output matters most."""
    text = "97% tests passed, 3 tests failed out of 482\n"
    assert check_skips.parse_total(text) == 482


def test_the_floor_is_below_the_real_suite_and_above_a_collapse():
    """The floor has to be able to fire, and must not fire on the real suite.

    A floor set above the current count would fail every run; one set at zero
    would never fire. Both are guards whose output is the same when they are
    absent, so the value is asserted against the suite it guards.
    """
    floor = check_skips.DEFAULT_MIN_TESTS
    assert floor > 0
    # The C++ suite is 482 tests as of Session H. The floor must leave room for
    # tests to be added without maintenance, and still refuse a run that
    # collapsed to a handful.
    assert floor <= 482
    assert floor > 100
