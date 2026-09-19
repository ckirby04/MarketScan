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

from research.parity import (
    EXPOSURE_OFFSET,
    EXPOSURE_SCALE,
    FIXTURE_BARS,
    FIXTURE_EQUITY,
    MICROS,
    PARITY_SPEC,
    ParityConfig,
    assert_no_ties,
    decode_exposure,
    encode_exposure,
    fixture_bars,
    notional,
    simulate,
)


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
    two implementations and look like an accounting bug."""
    assert_no_ties(bars)


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

    for date, equity in run.equity.items():
        held = run.positions.loc[date]
        held = held[held != 0.0]
        marked = sum(notional(float(closes.loc[date, symbol]), float(quantity))
                     for symbol, quantity in held.items())
        assert equity == run.cash.loc[date] + marked


def test_cash_moves_only_on_rebalance_dates_and_the_first_one_is_neutral(bars):
    """Two properties, and the first draft of this test asserted a third that
    was not true.

    Cash can only change when the book trades, so between rebalances it is flat.
    And the *opening* rebalance is cash-neutral to the micro-dollar, because
    equal notional long and short at the same prices nets to nothing under
    optimistic fills -- which is a sign check on the ledger that nothing else
    here performs.

    Later rebalances are *not* cash-neutral, which is what the original version
    of this test got wrong: by then the old positions have drifted away from
    equal notional, so closing them releases an unequal amount of cash.
    """
    config = ParityConfig()
    run = simulate(bars, config)
    dates = list(run.cash.index)

    assert run.cash.iloc[0] == pytest.approx(100_000 * MICROS, abs=8)

    for index, date in enumerate(dates):
        if index == 0:
            continue
        if index % config.rebalance_sessions != 0:
            assert run.cash.iloc[index] == run.cash.iloc[index - 1], f"cash moved on {date}"

    assert run.cash.nunique() > 1, "cash never moved at all; the book is not rebalancing"


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
