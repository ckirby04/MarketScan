"""M9: the Python half of the parity pair, and the fixture both halves read.

The cross-language comparison lives in the C++ suite, because that is where the
engine is. What is checked here is everything the comparison depends on: that the
fixture is reproducible, that the reference accounting is the engine's accounting,
and that the fixture cannot silently drift from the curve committed beside it.
"""

from __future__ import annotations

import math

import numpy as np
import pandas as pd
import pytest

from research.delisting import APPLY_TRUE_RETURN, LAST_PRICE, MODELS, applied_returns
from research.panel import generate_panel
from research.parity import (
    CHURN_APPLIED,
    CHURN_BARS,
    CHURN_DELISTINGS,
    CHURN_EQUITY,
    CHURN_MEMBERSHIP,
    CHURN_UNIVERSE,
    EXPOSURE_OFFSET,
    EXPOSURE_SCALE,
    FIXTURE_BARS,
    FIXTURE_EQUITY,
    MICROS,
    PARITY_CHURN_SPEC,
    PARITY_SPEC,
    ROLL_SYMBOL,
    ParityConfig,
    assert_no_gaps,
    assert_no_ties,
    assert_roll_symbol_never_trades,
    churn_membership,
    decode_exposure,
    encode_exposure,
    fixture_bars,
    notional,
    simulate,
)

# pd.read_csv's arguments for reading doubles back bit for bit.
EXACT_FLOATS = {"float_precision": "round_trip"} if int(pd.__version__.split(".")[0]) < 3 else {}


def test_the_encoding_round_trips():
    values = np.array([-8.5, -1.0, 0.0, 1e-6, 0.25, 3.75, 8.5])
    recovered = decode_exposure(encode_exposure(values))
    assert np.allclose(recovered, values, atol=1e-6)

    # Positive and integral, because it travels through an integer CSV column.
    encoded = encode_exposure(values)
    assert (encoded > 0).all()
    assert np.allclose(encoded, np.round(encoded))


def test_notional_rounds_half_away_from_zero_like_the_engine():
    """`np.round` is banker's rounding and disagrees with std::llround on every
    exact half. One micro-dollar is one more than an exact parity test allows."""
    assert notional(0.5e-6, 1.0) == 1
    assert notional(-0.5e-6, 1.0) == -1
    assert notional(1.5e-6, 1.0) == 2
    assert notional(2.5e-6, 1.0) == 3       # np.round would give 2
    assert notional(-2.5e-6, 1.0) == -3

    assert notional(10.0, 3.0) == 30_000_000
    with pytest.raises(ValueError):
        notional(float("inf"), 1.0)


@pytest.fixture(scope="module")
def bars():
    return fixture_bars()


def test_the_fixture_has_no_exposure_ties(bars):
    """A tie would put a name on opposite sides of the decile boundary in the
    two implementations and look like an accounting bug.

    The count is asserted as well as the absence of ties. `assert_no_ties` raises
    on a tie and returns quietly otherwise -- including when it examined nothing
    at all, which an empty bar table or a rebalance interval longer than the
    panel would both produce. Six rebalance dates in 126 sessions at 21.
    """
    assert assert_no_ties(bars) == 6


def test_the_fixture_is_churn_free(bars):
    """Parity is asserted exactly, which is only legitimate with no delistings:
    the two layers model those differently, and an expected gap is what a real
    bug would hide behind."""
    assert PARITY_SPEC.delist_hazard == 0.0
    per_date = bars.groupby("date")["symbol"].nunique()
    assert (per_date == PARITY_SPEC.n_securities).all()


def test_the_reference_run_actually_trades(bars):
    """A strategy that never traded would make parity true for the wrong
    reason."""
    run = simulate(bars)
    assert run.trades > 20
    assert (run.positions.abs().sum(axis=1) > 0).any()


def test_the_accounting_identity_holds_every_day(bars):
    """equity == cash + the marked value of every position, exactly.

    The same identity the C++ ledger asserts after every fill. Exactly, not to a
    tolerance: that is what integer micro-dollars are for.
    """
    run = simulate(bars)
    closes = bars.pivot(index="date", columns="symbol", values="close")

    assert len(run.equity) == len(closes.index) > 100

    for date, equity in run.equity.items():
        held = run.positions.loc[date]
        held = held[held != 0.0]
        marked = sum(notional(float(closes.loc[date, symbol]), float(quantity))
                     for symbol, quantity in held.items())
        assert equity == run.cash.loc[date] + marked


def test_cash_moves_the_session_after_a_rebalance(bars):
    """A rebalance on session N moves cash at N+1, and this has been right for two
    different reasons.

    It used to be a *sampling* fact: the decision on session N filled at session
    N's prices while the engine was dispatching N+1's first bar, and the snapshot
    was taken pre-trade, so the trade surfaced one row later. Since Session D Part
    3 it is an *economic* fact: session N's cross-section sets targets, and those
    targets do not become orders until each name next prints, which is session
    N+1. The observable is identical and the mechanism is not.

    Two earlier drafts of this test asserted cash never moves at all (true only of
    the opening rebalance) and that it moves on rebalance dates (true until the
    sampling instant was aligned with the engine's).
    """
    config = ParityConfig()
    run = simulate(bars, config)

    assert run.cash.iloc[0] == pytest.approx(100_000 * MICROS, abs=8)

    for index in range(1, len(run.cash)):
        traded_yesterday = (index - 1) % config.rebalance_sessions == 0
        if not traded_yesterday:
            assert run.cash.iloc[index] == run.cash.iloc[index - 1], (
                f"cash moved at index {index} with no rebalance the session before")

    assert run.cash.nunique() > 1, "cash never moved at all; the book is not rebalancing"


def test_nothing_is_held_on_the_opening_session(bars):
    """The sharpest case for the release convention.

    Session 0 is the first cross-section, so it can only *set* targets; the
    earliest price any of them can trade at is session 1's. The record for session
    0 is therefore an empty book and full cash, which is exactly what the engine
    reports -- it has not acted yet, and under the old semantics it had, at prices
    it was still looking at.
    """
    run = simulate(bars, ParityConfig())
    first = run.positions.iloc[0]

    assert (first.abs() < 1e-12).all(), "session 0 recorded a book before the engine has one"
    assert run.cash.iloc[0] == pytest.approx(100_000 * MICROS, abs=8)
    assert run.equity.iloc[0] == run.cash.iloc[0]

    # And by the next session the book is there.
    assert (run.positions.iloc[1].abs() > 0).any()


def test_crossing_the_spread_costs_money(bars):
    """The default fill model is worse than the optimistic one, and by roughly
    the spread times what was traded. The exact figure is the C++ side's to
    assert; here it only has to go the right way."""
    optimistic = simulate(bars, ParityConfig(optimistic=True))
    crossing = simulate(bars, ParityConfig(optimistic=False))

    assert crossing.equity.iloc[-1] < optimistic.equity.iloc[-1]
    assert crossing.trades == optimistic.trades


def test_commission_is_charged_per_share_when_asked(bars):
    free = simulate(bars, ParityConfig(commission_per_share=0.0))
    charged = simulate(bars, ParityConfig(commission_per_share=0.005))

    assert free.commission == 0
    assert charged.commission > 0
    assert charged.equity.iloc[-1] < free.equity.iloc[-1]


def test_the_reference_is_deterministic(bars):
    first = simulate(bars)
    second = simulate(bars)
    pd.testing.assert_series_equal(first.equity, second.equity)


# ---------------------------------------------------------------------------
# The committed fixture
# ---------------------------------------------------------------------------


@pytest.mark.skipif(not FIXTURE_BARS.exists(), reason="the fixture has not been written")
def test_the_committed_bars_are_what_the_generator_produces(bars):
    """Regenerating the fixture must reproduce it byte for byte, or the C++ test
    and this one are reading different data."""
    committed = pd.read_csv(FIXTURE_BARS, parse_dates=["date"])
    assert len(committed) == len(bars)
    assert list(committed["symbol"]) == list(bars["symbol"])
    assert np.allclose(committed["close"], bars["close"], atol=5e-5)
    assert (committed["volume"].to_numpy() == bars["volume"].round().to_numpy()).all()


@pytest.mark.skipif(not FIXTURE_EQUITY.exists(), reason="the fixture has not been written")
def test_the_committed_curve_matches_a_fresh_run():
    """The curve the C++ test asserts against is the one this code produces now.

    If this fails, either the reference implementation changed or the fixture
    did, and the cross-language assertion is comparing the engine to a stale
    number -- which would pass or fail for reasons that have nothing to do with
    the engine.
    """
    committed = pd.read_csv(FIXTURE_EQUITY, parse_dates=["date"]).set_index("date")
    fresh = simulate(pd.read_csv(FIXTURE_BARS, parse_dates=["date"]))

    assert len(committed) == len(fresh.equity)
    assert (committed["equity_micros"].to_numpy() == fresh.equity.to_numpy()).all()
    assert (committed["cash_micros"].to_numpy() == fresh.cash.to_numpy()).all()


@pytest.mark.skipif(not FIXTURE_BARS.exists(), reason="the fixture has not been written")
def test_the_committed_volume_decodes_into_an_exposure():
    """The C++ theory stands down on bars whose volume is not an encoded
    exposure. The fixture has to satisfy that check, and real bars must not."""
    committed = pd.read_csv(FIXTURE_BARS)
    decoded = decode_exposure(committed["volume"].to_numpy())
    assert np.abs(decoded).max() < 9.0

    # A real volume column decodes to nonsense, which is the point of the guard.
    real = decode_exposure(np.array([1_000_000.0 * 2.5, 850_000.0]))
    assert (np.abs(real) > 9.0).any()
# ---------------------------------------------------------------------------
# The churn fixture
# ---------------------------------------------------------------------------
#
# Session D Part 2. The cross-language comparison is still in the C++ suite; what
# is checked here is everything it depends on, plus the two properties that make
# the churn case mean anything: that the delisting return is separable from the
# price series, and that the symbol whose bar rolls each session is never traded.


@pytest.fixture(scope="module")
def churn_panel():
    return generate_panel(PARITY_CHURN_SPEC)


@pytest.fixture(scope="module")
def churn_bars():
    return pd.read_csv(CHURN_BARS, parse_dates=["date"])


def test_the_churn_fixture_actually_churns(churn_panel, churn_bars):
    assert len(churn_panel.delistings) > 10
    assert len(churn_panel.securities) > churn_panel.spec.n_securities

    # Every session has the same number of names: a departure is replaced, so the
    # panel never shrinks. A shrinking panel would be survivorship bias introduced
    # by the generator.
    per_session = churn_bars.groupby("date")["symbol"].nunique()
    assert per_session.nunique() == 1


def test_the_departure_return_is_not_in_the_price_series(churn_panel):
    """The change that made the churn case worth running.

    A departing name's final bar is an ordinary bar. If the delisting return were
    compounded into it, the engine's "liquidate at the last price the book has"
    would capture the loss by construction and the two layers could agree
    perfectly while being wrong in the same direction about the size of the exit.
    """
    returns = churn_panel.wide("ret")
    assert len(churn_panel.delistings) > 10

    worst_departing = []
    for security, row in churn_panel.delistings.iterrows():
        final = returns.loc[row["date"], security]
        assert np.isfinite(final)
        worst_departing.append(float(final))
        # A -100% delisting return exists in this fixture; no bar carries it.
        assert final > -0.5, (f"{security}'s final bar carries {final:+.2%}, which is the "
                              f"delisting return leaking back into the price series")

    # And the returns the table carries really are severe, so the assertion above
    # is not passing because there was nothing to leak.
    assert churn_panel.delistings["delisting_return"].min() <= -0.9
    assert min(worst_departing) > -0.5


def test_both_causes_are_represented_and_bounded(churn_panel):
    causes = set(churn_panel.delistings["cause"])
    assert causes == {"distress", "voluntary"}

    values = churn_panel.delistings["delisting_return"]
    assert values.min() >= -1.0, "a delisting cannot lose more than everything"
    distress = churn_panel.delistings.loc[churn_panel.delistings["cause"] == "distress",
                                          "delisting_return"]
    assert (distress <= 0.0).all(), "a distressed delisting cannot pay a premium"


def test_the_committed_membership_is_what_the_bars_say(churn_bars):
    """One source, two readers.

    The engine is handed the membership file and the reference is handed the bars.
    Anything that made them disagree would surface as a parity failure with
    nothing wrong on either side.
    """
    committed = pd.read_csv(CHURN_MEMBERSHIP, parse_dates=["first_session", "last_session"])
    derived = churn_membership(churn_bars)

    pd.testing.assert_frame_equal(
        committed.sort_values("symbol").reset_index(drop=True),
        derived.sort_values("symbol").reset_index(drop=True))


def test_the_committed_delisting_table_covers_exactly_the_names_that_left(churn_panel):
    committed = pd.read_csv(CHURN_DELISTINGS)
    assert set(committed["symbol"]) == set(churn_panel.delistings.index)

    # And nothing else. A survivor with a row of zero would turn "nobody measured
    # this" into "this cost nothing", which is the distinction the model exists to
    # keep.
    assert ROLL_SYMBOL not in set(committed["symbol"])


def test_the_fixture_has_no_gaps(churn_bars):
    assert_no_gaps(churn_bars)

    # And the assertion bites: drop one interior session for one name.
    victim = "SY0005"
    dates = sorted(churn_bars.loc[churn_bars["symbol"] == victim, "date"])
    assert len(dates) > 4
    holed = churn_bars.drop(churn_bars[(churn_bars["symbol"] == victim) &
                                       (churn_bars["date"] == dates[2])].index)
    with pytest.raises(ValueError, match="gap"):
        assert_no_gaps(holed)


def test_the_rolling_symbol_is_never_traded(churn_bars):
    """The property both fixtures depend on and neither used to state.

    The engine applies a bar to its book before showing it to anybody, and the
    shim fires `on_session_close(N)` from inside the dispatch of the first bar of
    session N+1 -- so an order in that one symbol is sized against session N's
    close and filled at N+1's. See `research.parity.ROLL_SYMBOL`.
    """
    assert assert_roll_symbol_never_trades(churn_bars) == ROLL_SYMBOL

    # The churn-free fixture satisfies it too, by accident rather than by design,
    # which is exactly why it is now checked.
    assert assert_roll_symbol_never_trades(fixture_bars()) == "SY0000"


def test_the_rolling_symbol_check_bites(churn_bars):
    """A roll symbol ranked into a decile is caught."""
    tampered = churn_bars.copy()
    roll = tampered["symbol"] == ROLL_SYMBOL
    # The highest exposure in the panel, so it lands in the long leg.
    tampered.loc[roll, "volume"] = float(encode_exposure(8.5))
    with pytest.raises(ValueError, match="session ahead"):
        assert_roll_symbol_never_trades(tampered)


def test_a_position_held_into_a_departure_is_liquidated_and_the_return_booked(churn_bars,
                                                                             churn_panel):
    assumed = simulate(churn_bars)
    honest = simulate(churn_bars,
                      delisting_returns=applied_returns(churn_panel.delistings,
                                                        APPLY_TRUE_RETURN))

    # The same departures, on the same sessions, at the same prices.
    assert assumed.forced_exits == honest.forced_exits >= 5

    # `last_price` books nothing at all. That is the whole of that model.
    assert assumed.delisting_log == []
    assert assumed.delisting_pnl == 0

    assert len(honest.delisting_log) == honest.forced_exits
    assert honest.delisting_pnl != 0

    # Both signs, and one total loss, so neither the credit nor the debit path is
    # untested and the worst case is covered.
    amounts = [row[5] for row in honest.delisting_log]
    assert min(amounts) < 0 < max(amounts)
    assert any(row[4] == -1.0 for row in honest.delisting_log)

    # Every booked adjustment is the position as it stood when the name left,
    # marked at the last price it printed.
    for date, symbol, quantity, mark, applied, micros in honest.delisting_log:
        assert micros == notional(mark * applied, quantity)
        assert applied == pytest.approx(
            float(churn_panel.delistings.loc[symbol, "delisting_return"]))
        # The position is gone by the next session, not merely adjusted.
        assert symbol not in honest.positions.loc[date][
            honest.positions.loc[date] != 0.0].index


def test_the_assumption_flatters_the_run_and_the_gap_exceeds_the_adjustments(churn_bars,
                                                                            churn_panel):
    """Session D Part 2.3, from the Python side.

    The engine measures this independently through its own ledger; the two are a
    cross-check rather than a restatement.
    """
    assumed = simulate(churn_bars)
    honest = simulate(churn_bars,
                      delisting_returns=applied_returns(churn_panel.delistings,
                                                        APPLY_TRUE_RETURN))

    start = ParityConfig().starting_cash * MICROS
    assumed_end = int(assumed.equity.iloc[-1])
    honest_end = int(honest.equity.iloc[-1])

    assert assumed_end > start > honest_end, (
        "the assumption is supposed to turn a losing run into a winning one on this "
        "fixture; it no longer does")

    gap = assumed_end - honest_end
    assert gap > 10 * MICROS * 1000, f"the assumption was worth only {gap / MICROS:,.2f}"

    # Strictly more than the adjustments themselves. A delisting return moves cash,
    # decision-time equity is cash plus the marked book, and every rebalance after
    # the first booked loss sizes a smaller book -- so the gap is the adjustments
    # plus everything the smaller book then failed to earn.
    assert gap > -honest.delisting_pnl


def test_the_committed_churn_curves_match_a_fresh_run(churn_bars, churn_panel):
    """The curves the C++ suite asserts against cannot drift from this code.

    Simulated from the committed CSV, not from the frame that produced it: the file
    carries four decimals and the panel carries a double, and a curve computed
    before writing is a curve for prices the engine will never see.
    """
    assert len(MODELS) == 2, "a delisting model was added or removed"

    for model in MODELS:
        committed = pd.read_csv(CHURN_EQUITY[model])
        fresh = simulate(churn_bars,
                         delisting_returns=applied_returns(churn_panel.delistings, model))

        assert len(committed) == len(fresh.equity)
        assert list(committed["equity_micros"]) == list(fresh.equity.astype("int64"))
        assert list(committed["cash_micros"]) == list(fresh.cash.astype("int64"))


def test_the_committed_applied_log_matches_a_fresh_run(churn_bars, churn_panel):
    # An exact float reader is load-bearing. pandas 2's default C converter is fast
    # and inexact: it parses the committed `287.08291392594157` to a different
    # double than the one that wrote it, and this test failed on the last bit
    # because of the reader rather than because of anything it was checking.
    # The C++ side reads these with `strtod`, which is exact. pandas 3 reads
    # exactly by default and deprecates `float_precision`, which the warning
    # filter turns into a failure, so the argument goes only to pandas 2.
    committed = pd.read_csv(CHURN_APPLIED, **EXACT_FLOATS)
    fresh = simulate(churn_bars,
                     delisting_returns=applied_returns(churn_panel.delistings,
                                                       APPLY_TRUE_RETURN))

    assert len(committed) == len(fresh.delisting_log)
    for (_, want), got in zip(committed.iterrows(), fresh.delisting_log):
        assert want["symbol"] == got[1]
        # Bit-exact, not approximate: the C++ side is asserted to have computed the
        # identical adjustment from these two doubles.
        assert float(want["quantity"]) == got[2]
        assert float(want["mark"]) == got[3]
        assert float(want["delisting_return"]) == got[4]
        assert int(want["micros"]) == got[5]


def test_a_delisting_return_nobody_supplied_is_not_zero(churn_bars, churn_panel):
    """A security the table says nothing about is liquidated at the last price
    under either model, which is not the same as one measured to have cost
    nothing."""
    partial = applied_returns(churn_panel.delistings, APPLY_TRUE_RETURN)
    dropped = sorted(partial)[0]
    del partial[dropped]

    full = simulate(churn_bars,
                    delisting_returns=applied_returns(churn_panel.delistings,
                                                      APPLY_TRUE_RETURN))
    thinner = simulate(churn_bars, delisting_returns=partial)

    # Same forced exits either way -- coverage changes what an exit was worth, not
    # whether it happened.
    assert thinner.forced_exits == full.forced_exits
    assert len(thinner.delisting_log) <= len(full.delisting_log)


def test_applied_returns_is_empty_under_the_default(churn_panel):
    assert applied_returns(churn_panel.delistings, LAST_PRICE) == {}
    assert applied_returns(churn_panel.delistings) == {}, "the default is not last_price"
    assert len(applied_returns(churn_panel.delistings, APPLY_TRUE_RETURN)) == len(
        churn_panel.delistings)
# ---------------------------------------------------------------------------
# The target book
# ---------------------------------------------------------------------------
#
# Session D Part 3. A cross-sectional decision is taken from a session's closes
# and cannot honestly trade at them, so both layers state a target book and each
# name's target becomes an order when that name next prints.
#
# The committed-curve tests above are the strong check -- they hold this reference
# against an engine that implements the same rule independently. These assert the
# rule itself, so that a failure says which part of it broke.


def test_a_target_is_filled_at_the_next_session_not_the_one_it_was_sized_from(bars):
    """The whole of the fix, in one assertion.

    The first rebalance is session 0. Nothing trades on session 0 -- the earliest
    price the decision can reach is session 1's -- and every one of the session-1
    fills is at a session-1 close.
    """
    run = simulate(bars, ParityConfig())
    closes = bars.pivot(index="date", columns="symbol", values="close")
    sessions = list(closes.index)

    traded_on = sorted({date for date, _, _, _ in run.trade_log})
    assert sessions[0] not in traded_on, (
        "something traded on the opening session, which is the session the first "
        "decision was made from")
    assert sessions[1] in traded_on

    # Every logged price is that session's close for that symbol, never the
    # previous session's.
    for date, symbol, _shares, price in run.trade_log:
        assert price == pytest.approx(float(closes.loc[date, symbol]), abs=0.0)

    # And the share counts were sized against the *previous* session's close, so
    # the two genuinely differ -- if they did not, this test would be passing
    # because the prices happened to be equal.
    first = [row for row in run.trade_log if row[0] == sessions[1]]
    assert first
    differed = sum(1 for _d, symbol, _s, price in first
                   if price != float(closes.loc[sessions[0], symbol]))
    assert differed == len(first), (
        "every fill price equalled the close it was sized from; the release is not "
        "happening a session later")


def test_targets_are_released_in_symbol_order(bars):
    """The order the engine's bars arrive in, which is the order it releases in.

    It matters only if cash binds -- it does not on a dollar-neutral book of this
    size -- but the two layers have to agree about it, so it is asserted rather
    than assumed.
    """
    run = simulate(bars, ParityConfig())
    assert run.trade_log, "nothing traded, so release order is not being checked"

    by_session: dict[object, list[str]] = {}
    for date, symbol, _shares, _price in run.trade_log:
        by_session.setdefault(date, []).append(symbol)

    assert by_session
    for date, symbols in by_session.items():
        assert symbols == sorted(symbols), f"releases out of symbol order on {date}"


def test_a_target_whose_name_never_prints_again_is_abandoned(churn_bars, churn_panel):
    """A target from two sessions ago, filled now, is a decision acting on
    information it no longer has.

    A departing name is in the cross-section of its final session, so it collects a
    target it can never reach whenever that session is a rebalance one. The engine
    drops it in `CrossSectionalStrategy::abandon_targets`; this drops it too, and
    the count is what makes the two comparable.
    """
    run = simulate(churn_bars,
                   delisting_returns=applied_returns(churn_panel.delistings,
                                                     APPLY_TRUE_RETURN))
    assert run.targets_abandoned > 0

    # Nothing but the sweep trades in a name after its last bar.
    #
    # The sweep itself does, necessarily: a forced exit is the host liquidating at
    # the last price the book held, which happens on the first session the name is
    # no longer a member. So the count of trades dated after a name's final bar has
    # to be exactly the count of forced exits -- one more than that would be a
    # target released against a price that no longer exists, which is the failure
    # this abandonment exists to prevent.
    last_bar = churn_bars.groupby("symbol")["date"].max()
    after_the_end = [(date, symbol) for date, symbol, _s, _p in run.trade_log
                     if date > last_bar[symbol]]
    assert len(after_the_end) == run.forced_exits, (
        f"{len(after_the_end)} trades landed after their name's last bar against "
        f"{run.forced_exits} forced exits: {after_the_end}")

    # And each one is a liquidation, priced at that name's final close.
    closes = churn_bars.pivot(index="date", columns="symbol", values="close")
    for date, symbol, shares, price in run.trade_log:
        if date <= last_bar[symbol]:
            continue
        assert price == pytest.approx(float(closes.loc[last_bar[symbol], symbol]), abs=0.0)
        assert shares != 0.0

    # The churn-free fixture abandons nothing, because nobody leaves it.
    assert simulate(fixture_bars()).targets_abandoned == 0


def test_the_two_membership_spellings_agree(churn_bars):
    """Intervals and snapshots describe the same universe.

    `parity_churn_membership.csv` is `symbol,first_session,last_session`, which
    is what a reader and the Python reference want. `parity_churn_universe.csv`
    is `date,TICKER,TICKER,...`, which is the shape real membership data comes in
    and what the engine's `SnapshotMembershipSource` reads -- it derives the
    intervals back out by diffing consecutive days.

    Both are written from the same bars, so the Universe Inspector and the parity
    tests cannot be looking at different universes. This is the assertion that
    says so.
    """
    intervals = pd.read_csv(CHURN_MEMBERSHIP, parse_dates=["first_session", "last_session"])

    snapshots: dict[pd.Timestamp, set[str]] = {}
    with CHURN_UNIVERSE.open(encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("#") or line.startswith("date,"):
                continue
            fields = line.strip().split(",")
            if len(fields) < 2:
                continue
            snapshots[pd.Timestamp(fields[0])] = set(fields[1:])

    sessions = sorted(churn_bars["date"].unique())
    assert len(snapshots) == len(sessions)

    # Every snapshot is exactly the names whose interval covers that date.
    for date in sessions:
        covering = set(intervals.loc[(intervals["first_session"] <= date) &
                                     (intervals["last_session"] >= date), "symbol"])
        assert snapshots[pd.Timestamp(date)] == covering, f"the two disagree on {date}"

    # And the snapshots really do change, or this would pass on a static universe.
    assert len({frozenset(members) for members in snapshots.values()}) > 5
