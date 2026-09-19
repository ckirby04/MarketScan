"""The sizing rule, cross-tested against the other implementation.

`docs/SIZING.md` is the specification and
`include/marketscan/strategy/sizing.hpp` is the other implementation of it. Both
suites read `data/samples/sizing_cases.csv`, so neither can drift onto a
different grid, and both assert exact equality rather than closeness: two
implementations of one written rule, evaluated in the order the rule gives, on
identical inputs, produce identical doubles.
"""

from __future__ import annotations

import math

import pytest

from research.sizing import (
    CASES,
    MICROS,
    MINIMUM_TRADE,
    Case,
    decision_equity,
    grid,
    notional_micros,
    per_name_notional,
    read_cases,
    should_submit,
    side_count,
    target_shares,
    trade_delta,
    write_cases,
)


def test_the_committed_grid_is_what_the_generator_produces():
    """If the file and the generator disagree, the C++ suite is reading numbers
    this implementation no longer produces."""
    committed = read_cases()
    fresh = grid()

    assert len(committed) == len(fresh)
    for (case, expected), regenerated in zip(committed, fresh):
        assert case == regenerated, case.name
        out = regenerated.evaluate()
        assert out["side"] == expected["side"], case.name
        for key in ("per_name", "target", "delta"):
            assert out[key] == expected[key], f"{case.name}: {key}"
        assert out["submit"] == expected["submit"], case.name


def test_the_grid_is_round_trip_exact(tmp_path):
    """%.17g recovers a binary64 exactly. Anything less and both suites would be
    asserting that the CSV writer and the parser agree."""
    path = write_cases(tmp_path / "cases.csv")
    for case, expected in read_cases(path):
        out = case.evaluate()
        for key in ("per_name", "target", "delta"):
            assert out[key] == expected[key], f"{case.name}: {key}"


def test_the_grid_covers_the_adversarial_cases_the_spec_names():
    cases = [case for case, _ in read_cases()]
    assert len(cases) >= 40

    assert any(side_count(c.count, c.decile) == 0 for c in cases), "no stand-down case"
    assert any(0.0 < c.close <= 1e-4 for c in cases), "no near-zero price"
    assert any(c.equity_micros < 10 for c in cases), "no rounding-boundary equity"
    assert any(not c.long_side for c in cases), "no short side"
    assert any(c.position != 0.0 for c in cases), "no already-held position"


# ---------------------------------------------------------------------------
# The steps, individually
# ---------------------------------------------------------------------------


def test_notional_rounds_half_away_from_zero():
    assert notional_micros(0.5e-6, 1.0) == 1
    assert notional_micros(-0.5e-6, 1.0) == -1
    assert notional_micros(2.5e-6, 1.0) == 3       # banker's rounding would give 2
    assert notional_micros(-2.5e-6, 1.0) == -3
    assert notional_micros(10.0, 3.0) == 30_000_000

    with pytest.raises(ValueError):
        notional_micros(float("inf"), 1.0)


def test_a_cross_section_too_small_for_two_legs_sizes_nothing():
    assert side_count(1, 0.1) == 0
    assert side_count(0, 0.1) == 0
    assert side_count(2, 0.5) == 1
    assert side_count(1, 0.5) == 0
    assert side_count(30, 0.0) == 0
    assert side_count(30, 0.1) == 3


def test_decision_equity_marks_at_this_sessions_closes():
    """Step 2, and the whole reason SIZING.md exists.

    The engine's `equity()` marks one symbol per session at the *next* session's
    close, because on_session_close fires while the next bar is being dispatched.
    Marking from the cross-section removes both the disagreement and the
    lookahead.
    """
    marks = {"A": 10.0, "B": 20.0}
    positions = {"A": 100.0, "B": -50.0}

    equity = decision_equity(100_000 * MICROS, marks, positions)
    assert equity == 100_000 * MICROS + notional_micros(10.0, 100.0) + \
        notional_micros(20.0, -50.0)

    # A position with no mark in this session's cross-section is skipped rather
    # than marked at something else: it did not trade, so there is no close.
    assert decision_equity(0, {"A": 10.0}, {"A": 1.0, "GONE": 5.0}) == notional_micros(10.0, 1.0)


def test_the_target_is_fractional_and_signed():
    per_name = per_name_notional(100_000 * MICROS, 1.0, 3)
    assert per_name == pytest.approx(16_666.666666666668)

    long_target = target_shares(per_name, 39.5234, True)
    short_target = target_shares(per_name, 39.5234, False)
    assert long_target > 0 and short_target < 0
    assert long_target == -short_target
    assert long_target != math.floor(long_target), "shares are fractional by design"

    with pytest.raises(ValueError):
        target_shares(per_name, 0.0, True)


def test_the_minimum_trade_is_a_floor_not_a_rounding():
    assert should_submit(MINIMUM_TRADE)
    assert not should_submit(MINIMUM_TRADE / 2)
    assert not should_submit(0.0)
    assert should_submit(-1.0)

    assert trade_delta(10.0, 3.0) == 7.0
    assert trade_delta(3.0, 10.0) == -7.0


def test_scaling_gross_scales_the_book_linearly():
    single = per_name_notional(100_000 * MICROS, 1.0, 3)
    double = per_name_notional(100_000 * MICROS, 2.0, 3)
    assert double == 2.0 * single
    assert per_name_notional(100_000 * MICROS, 1.0, 0) == 0.0


def test_the_committed_grid_is_where_the_84_session_case_lives():
    """The divergence that produced SIZING.md has a named row in the grid."""
    names = [case.name for case, _ in read_cases()]
    assert any(name.startswith("fixture/") for name in names)
    assert any("306.417" in name or name.startswith("fixture/pos306") for name in names), \
        f"the fixture's own share count is not in the grid: {names[:5]}"
