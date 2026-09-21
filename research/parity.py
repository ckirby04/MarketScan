"""M9: the same strategy, implemented twice, and made to agree.

    python -m research.parity --write-fixture

Spec 2 M9. Long the top decile of the synthetic exposure, short the bottom,
rebalanced monthly -- written once as a C++ theory (`theories/decile_parity/`)
and once here. Same panel, same rule, two entirely separate implementations of
the accounting. Under optimistic fills and zero costs the equity curves have to
match to floating-point tolerance, and if they do not, one of them is wrong.

CHURN-FREE FIRST, AND THAT ORDERING IS THE POINT

On a panel where nothing leaves the universe there is **no legitimate source of
disagreement**. The two layers model delistings differently -- the engine
force-liquidates at the last price, this compounds the departure return -- so a
panel with churn has an expected gap, and an expected gap is exactly the thing a
real bug hides behind. So the fixture has no churn, the parity assertion is
exact, and the churn case is measured separately.

WHY THIS DOES NOT USE portfolio.py

`portfolio.py` produces *active weights* and an active return series: a
benchmark-relative abstraction with no cash, no shares and no fills. The engine
produces an equity curve from a share-based ledger in integer micro-dollars.
Comparing those two would be comparing two different accounting models and
calling the difference a bug.

So this is a deliberate second implementation of the engine's accounting, in
Python, written from the same definitions:

    equity          = cash + sum over positions of notional(mark, quantity)
    notional(p, q)  = llround(p * q * 1e6)        micro-dollars, half away from zero
    cash            -= notional(fill_price, quantity) + commission

Integer micro-dollars, not floats, and the same rounding rule as
`include/marketscan/core/money.hpp`. Two implementations that agree to the last
micro-dollar agree because they are doing the same arithmetic, not because the
tolerance was generous.

THE SIGNAL TRAVELS IN THE VOLUME COLUMN

A theory sees bars and nothing else -- no files, no network, no side channel --
and the synthetic exposure is not a price. So the fixture encodes it into the one
bar field the execution layer never reads:

    volume = round((exposure + 10) * 1e6)

Both sides decode the same integer, so the ranking cannot differ in the last bit.
It is a fixture convention and `theories/decile_parity/decile_parity.hpp` refuses
to run on bars whose volume does not decode into an exposure's range.
"""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd

from research import sizing
from research.delisting import (APPLY_TRUE_RETURN, LAST_PRICE, MODELS, applied_returns)
from research.panel import Panel, PanelSpec, generate_panel

REPO = Path(__file__).resolve().parent.parent
FIXTURE_BARS = REPO / "data" / "samples" / "parity_panel.csv"
FIXTURE_EQUITY = REPO / "data" / "samples" / "parity_expected.csv"

CHURN_BARS = REPO / "data" / "samples" / "parity_churn_panel.csv"
CHURN_MEMBERSHIP = REPO / "data" / "samples" / "parity_churn_membership.csv"
CHURN_DELISTINGS = REPO / "data" / "samples" / "parity_churn_delistings.csv"
CHURN_EQUITY = {
    LAST_PRICE: REPO / "data" / "samples" / "parity_churn_expected_last_price.csv",
    APPLY_TRUE_RETURN: REPO / "data" / "samples" / "parity_churn_expected_apply_true_return.csv",
}
"""One committed curve per delisting model. Two files rather than one with a
column each, because the C++ side reads whichever the case under test selected
and a test that reads the wrong column would still parse."""

CHURN_APPLIED = REPO / "data" / "samples" / "parity_churn_applied.csv"
"""Every delisting return the reference run booked, with the position and the mark
it was measured against.

The equity curves agreeing is the assertion; this is what makes a disagreement
legible. A curve that parts company at session 90 says only that something is
wrong by then, and `Ledger::delisting_adjustments` compared against these rows
says which name, on which date, against what position, and by how much."""

EXPOSURE_OFFSET = 10.0
EXPOSURE_SCALE = 1_000_000.0
"""Must match `decile_parity.hpp`. Ten to keep the encoded value positive, a
million to carry six decimals through an integer CSV column."""

MICROS = 1_000_000

PARITY_SPEC = PanelSpec(
    n_securities=30,
    n_periods=126,
    lam=0.0008,
    seed=20260919,
    delist_hazard=0.0,
    n_sectors=3,
)
"""Six months, thirty names, **no churn**.

Sized against the repository's other committed sample: 3,780 rows, which is what
`synthetic_daily_5.csv` is, because a fixture that has to live in git should not
be the largest thing in the tree. Six monthly rebalances and a planted signal are
enough for the book to trade and to make money -- a strategy that never traded
would make parity true for the wrong reason."""

PARITY_CHURN_SPEC = PARITY_SPEC.replace(seed=20260920, delist_hazard=0.006,
                                        delist_exposure_tilt=-1.5)
"""The same fixture with names leaving it, which is the case the churn-free one
deliberately does not cover.

Thirty names, six months, the same planted signal; a different seed, because the
churn draws come from a stream the churn-free spec never touches. Two settings are
chosen rather than inherited, and both are fixture devices rather than claims
about the world:

**The hazard is tilted toward high exposure.** A departure in a name the book
never owned exercises nothing at all, and with a decile rule on thirty names only
six of the thirty are held at any instant -- so an untilted hazard spends most of
its departures on names nobody owns. At 0.004 with no tilt this fixture produced
twenty-three departures and exactly **one** held position, which would have made
the magnitude half of churn parity rest on a single row. A negative tilt raises the
hazard with the exposure, which is the long leg, and the committed fixture holds
seven departures across both causes -- including one total loss and one short
whose name left at a premium, so both signs of the adjustment are covered.

**The hazard is higher.** 0.006 rather than 0.004, for the same reason.

`write_churn_fixture` refuses to write a fixture where no position was ever held
into a departure, because that fixture would make churn parity pass by vacuity --
which is the specific failure Session D says not to report as a result."""


def encode_exposure(exposure: float | np.ndarray) -> np.ndarray:
    return np.round((np.asarray(exposure, dtype=float) + EXPOSURE_OFFSET) * EXPOSURE_SCALE)


def decode_exposure(volume: float | np.ndarray) -> np.ndarray:
    return np.asarray(volume, dtype=float) / EXPOSURE_SCALE - EXPOSURE_OFFSET


def notional(price: float, quantity: float) -> int:
    """Micro-dollars, rounded half away from zero. `Money::notional`, in Python.

    Re-exported from `research.sizing` so this module has one name for it and
    the rule has one implementation. See docs/SIZING.md.
    """
    return sizing.notional_micros(price, quantity)


@dataclass(frozen=True)
class ParityConfig:
    """The knobs both implementations have to be given identically."""

    decile: float = 0.1
    rebalance_sessions: int = 21
    gross: float = 1.0
    starting_cash: float = 100_000.0

    optimistic: bool = True
    """Fill at the mid. The engine's default crosses the spread; parity is
    asserted exactly under optimistic fills and as an inequality under the
    default, because the cost of crossing is the thing being measured."""

    spread_bps: float = 2.0
    """Full spread as a fraction of the close, when not optimistic. Matches
    `DailyBarFeed`'s default synthesized quote."""

    commission_per_share: float = 0.0
    """Zero for the parity assertion. The engine's default is 0.005 a share, and
    the parity test configures it to zero rather than trying to model it twice."""

    minimum_trade: float = 1e-9


@dataclass
class ParityRun:
    """What the reference implementation did, in the engine's own units."""

    equity: pd.Series
    """date -> equity in micro-dollars."""

    cash: pd.Series
    positions: pd.DataFrame
    """The book at the **end** of each session, which is when the engine's own
    sample is taken.

    A session's trades are the previous session's targets being released, so by
    the last bar of session N the book is whatever session N-1's cross-section
    asked for. Targets set from session N's own closes have not traded yet."""

    trades: int
    commission: int

    trade_log: list[tuple[pd.Timestamp, str, float, float]] = field(default_factory=list)
    """(date, symbol, shares, price) per fill. Recorded rather than reconstructed
    from position differences, which stopped being possible once the snapshots
    moved to pre-trade."""

    forced_exits: int = 0
    """Positions liquidated because the name left the panel."""

    delisting_log: list[tuple[pd.Timestamp, str, float, float, float, int]] = field(
        default_factory=list)
    """(date, symbol, quantity, mark, delisting_return, micro-dollars) per booked
    delisting return. The engine's `Ledger::delisting_adjustments`, in Python, and
    the two are compared row for row by `test_parity.cpp`."""

    delisting_pnl: int = 0
    """Micro-dollars booked by delisting returns rather than by fills. Zero under
    `last_price`, which is the whole of that model."""

    targets_abandoned: int = 0
    """Targets dropped because the symbol never printed again. The engine's
    `CrossSectionalStrategy::targets_abandoned`."""

    def to_frame(self) -> pd.DataFrame:
        return pd.DataFrame({"equity_micros": self.equity.astype("int64"),
                             "cash_micros": self.cash.astype("int64")})


def _fill_price(close: float, buying: bool, config: ParityConfig) -> float:
    if config.optimistic:
        return close
    half = close * config.spread_bps / 10_000.0 / 2.0
    return close + half if buying else close - half


def simulate(bars: pd.DataFrame, config: ParityConfig | None = None,
             delisting_returns: dict[str, float] | None = None) -> ParityRun:
    """Runs the decile rule over a bar table, in the engine's accounting.

    `bars` carries date, symbol, close and volume, with the exposure encoded in
    volume. Everything here mirrors `decile_parity.hpp` step for step: the same
    sort, the same decile boundary, closes before opens, the same epsilon on a
    trade too small to bother with.

    A TARGET BOOK, NOT ORDERS, AND WHY THE ORDER OF THE LOOP IS THE SPEC

    Session D Part 3. A cross-sectional decision is taken from a session's closes
    and cannot honestly trade at them, so `on_cross_section` states a target book
    and the shim turns each name's target into an order when that name **next
    prints**. Both layers now do that, so each session of this loop does four
    things in this order, and each one mirrors something the engine does:

    1. **Sweep.** A held name whose bars have ended is liquidated at the last price
       this table showed for it, then the delisting return the data supplies -- if
       any, and there is none under `last_price` -- is booked against the position
       that was closed. The engine does this from `on_event_complete` on the first
       event of the first session the name is no longer a member.

    2. **Release.** Every target set by the previous session's cross-section
       becomes a trade at **this** session's close, in ascending symbol order,
       which is the order a daily file's bars arrive in. A target whose symbol does
       not print is abandoned, not carried: a target from two sessions ago, filled
       now, is a decision acting on information it no longer has.

    3. **Record.** The book, cash and equity as of this session's closes. This is
       post-release and it has to be: the engine's own sample is the last state of
       the ledger before the clock moves on, and by then the session's releases
       have happened.

    4. **Decide.** On a rebalance session, rank this session's cross-section and
       set targets. No trade happens here.

    Steps 1 and 2 are written sweep-first, and the engine interleaves them -- it
    releases the session's *first* symbol before the sweep runs, because the sweep
    hangs off the first event's completion. The two orderings are identical only
    because the first symbol of every session is never traded, which is not an
    accident and not luck either: see `ROLL_SYMBOL` and
    `assert_roll_symbol_never_trades`.

    `delisting_returns` is what `research.delisting.applied_returns` produced, so
    it is empty under `last_price` and this function does not branch on the model.
    """
    config = config or ParityConfig()
    delisting_returns = delisting_returns or {}

    frame = bars.copy()
    frame["exposure"] = decode_exposure(frame["volume"].to_numpy())
    closes = frame.pivot(index="date", columns="symbol", values="close").sort_index()
    exposures = frame.pivot(index="date", columns="symbol", values="exposure").sort_index()

    cash = int(round(config.starting_cash * MICROS))
    shares: dict[str, float] = {}
    equity_by_date: dict[pd.Timestamp, int] = {}
    cash_by_date: dict[pd.Timestamp, int] = {}
    position_rows: dict[pd.Timestamp, dict[str, float]] = {}
    trades = 0
    commission_paid = 0
    trade_log: list[tuple[pd.Timestamp, str, float, float]] = []
    forced_exits = 0
    delisting_log: list[tuple[pd.Timestamp, str, float, float, float, int]] = []
    delisting_pnl = 0

    # Targets waiting for their symbol to print. `CrossSectionalStrategy`'s
    # target book, in Python.
    pending: dict[str, float] = {}
    abandoned = 0

    # The last close each name printed, and the last session it printed one.
    # Membership is read off the bar table rather than taken as a second input:
    # the engine is handed a membership file and this is handed the bars, and if
    # the two disagree the fixture is wrong in a way no parity assertion could
    # attribute. `write_churn_fixture` asserts they agree.
    last_close: dict[str, float] = {}
    final_session = {symbol: closes[symbol].last_valid_index() for symbol in closes.columns}

    for session, date in enumerate(closes.index):
        close_row = closes.loc[date].dropna()
        exposure_row = exposures.loc[date].reindex(close_row.index)

        # --- The sweep ---------------------------------------------------
        #
        # Anything held whose bars have ended is liquidated at the last price
        # this table showed for it, and then the delisting return the data
        # supplies -- if any, and there is none under `last_price` -- is booked
        # against the position that was closed.
        #
        # A held name with no bar today whose bars have NOT ended is a gap, not a
        # departure, and is left alone. The engine does the same thing for the
        # same reason: membership, not the presence of a print, is what says a
        # name has gone.
        for symbol in sorted(shares):
            if symbol in close_row.index or date <= final_session[symbol]:
                continue

            quantity = shares[symbol]
            mark = last_close[symbol]
            delta = sizing.trade_delta(0.0, quantity)
            fill = _fill_price(mark, buying=delta > 0, config=config)
            cash -= notional(fill, delta)
            charge = notional(config.commission_per_share, abs(delta))
            cash -= charge
            commission_paid += charge
            trade_log.append((date, symbol, delta, mark))
            trades += 1
            forced_exits += 1
            shares.pop(symbol)

            # After the exit, matching `sweep_removals`. The adjustment is
            # measured against the position as it stood when the name left, and
            # `mark * delisting_return` is formed first and rounded once -- the
            # same expression, in the same order, as `delisting_adjustment` in
            # include/marketscan/exec/delisting.hpp. Rounding it twice puts the
            # two layers a micro-dollar apart and the assertion has no tolerance.
            if symbol not in delisting_returns:
                continue
            applied = float(delisting_returns[symbol])
            amount = notional(mark * applied, quantity)
            cash += amount
            delisting_pnl += amount
            delisting_log.append((date, symbol, quantity, mark, applied, amount))

        # --- Release ------------------------------------------------------
        #
        # Ascending symbol order, because that is the order the engine's bars
        # arrive in and therefore the order its targets are released in. It
        # matters only if cash binds -- it does not on a dollar-neutral book of
        # this size -- but the two layers have to agree about it regardless.
        for symbol in sorted(pending):
            if symbol not in close_row.index:
                continue  # no print; abandoned below
            price = float(close_row[symbol])
            delta = sizing.trade_delta(pending[symbol], shares.get(symbol, 0.0))
            if not sizing.should_submit(delta):
                continue
            fill = _fill_price(price, buying=delta > 0, config=config)
            cash -= notional(fill, delta)
            charge = notional(config.commission_per_share, abs(delta))
            cash -= charge
            commission_paid += charge
            trade_log.append((date, symbol, delta, price))
            shares[symbol] = shares.get(symbol, 0.0) + delta
            if abs(shares[symbol]) < config.minimum_trade:
                shares.pop(symbol)
            trades += 1

        # Whatever is left never printed. Dropped, not carried, and counted --
        # `CrossSectionalStrategy::abandon_targets` does this at the top of every
        # session close, so a target lives exactly one session.
        abandoned += sum(1 for symbol in pending if symbol not in close_row.index)
        pending = {}

        for symbol, price in close_row.items():
            last_close[str(symbol)] = float(price)

        # --- Record -------------------------------------------------------
        #
        # After this session's releases and before its targets are set, because
        # that is the state the engine samples: one point per session, the last
        # state of the ledger before the clock moves on. By the last bar of a
        # session its releases have happened, and the targets this session's own
        # cross-section is about to produce have not traded.
        #
        # This moved with Session D Part 3. It used to be pre-trade, which was
        # right when the previous session's decision *filled* at the previous
        # session's prices; now that decision fills at this session's prices and
        # the snapshot has to sit after it. The whole committed curve moved with
        # it, which is the cost of the fix and is recorded in docs/STATUS.md.
        #
        # Marked from the last close each name printed, not from today's row.
        # The two are the same number for every name that printed today, which is
        # every name in a panel with no gaps -- but the engine marks from its
        # market book, which holds the last print regardless, and matching that is
        # free here and not free to discover later.
        marked = sum(notional(last_close[symbol], quantity)
                     for symbol, quantity in shares.items() if symbol in last_close)
        equity_by_date[date] = cash + marked
        cash_by_date[date] = cash
        position_rows[date] = dict(shares)

        if session % config.rebalance_sessions == 0 and not close_row.empty:
            # Descending by exposure, ties broken by symbol so that the decile
            # boundary cannot depend on an ordering the two implementations do
            # not share. `assert_no_ties` checks that the tiebreak never
            # actually has to arbitrate.
            order = sorted(close_row.index,
                           key=lambda symbol: (-exposure_row[symbol], symbol))
            count = len(order)
            side = sizing.side_count(count, config.decile)

            if side > 0:
                # Marked from this session's closes, per SIZING.md step 2 -- the
                # step the C++ side used to get from the engine's live book, one
                # symbol of which was already at the next session's price.
                marks = {symbol: float(close_row[symbol]) for symbol in close_row.index}
                equity_now = sizing.decision_equity(cash, marks, shares)
                per_name = sizing.per_name_notional(equity_now, config.gross, side)

                longs = set(order[:side])
                shorts = set(order[count - side:])

                # Targets, not trades. Sized against this session's close, which
                # is the price the ranking came from; filled at the next one. That
                # gap is the cost of not being able to trade at a close you are
                # still looking at, and assuming it away was the lookahead.
                #
                # Every name in the cross-section gets one, the middle included at
                # zero, exactly as the theory calls `set_flat` on them.
                for symbol in order:
                    price = float(close_row[symbol])
                    if symbol in longs:
                        pending[symbol] = sizing.target_shares(per_name, price, True)
                    elif symbol in shorts:
                        pending[symbol] = sizing.target_shares(per_name, price, False)
                    else:
                        pending[symbol] = 0.0

    return ParityRun(
        equity=pd.Series(equity_by_date, dtype="int64").sort_index(),
        cash=pd.Series(cash_by_date, dtype="int64").sort_index(),
        positions=pd.DataFrame(position_rows).T.sort_index().fillna(0.0),
        trades=trades,
        commission=commission_paid,
        trade_log=trade_log,
        forced_exits=forced_exits,
        delisting_log=delisting_log,
        delisting_pnl=delisting_pnl,
        targets_abandoned=abandoned,
    )


def fixture_bars(panel: Panel | None = None) -> pd.DataFrame:
    """The committed bar table: OHLCV with the exposure encoded in volume."""
    from research.export import bars_frame

    panel = panel or generate_panel(PARITY_SPEC)
    bars = bars_frame(panel)
    encoded = encode_exposure(panel.frame["exposure"].to_numpy())
    index = pd.MultiIndex.from_frame(bars[["date", "symbol"]].rename(
        columns={"symbol": "security"}))
    bars = bars.assign(volume=pd.Series(encoded, index=panel.frame.index).reindex(index).to_numpy())
    return bars


def assert_no_ties(bars: pd.DataFrame, config: ParityConfig | None = None) -> None:
    """No two names share an exposure on a rebalance date.

    The two implementations break ties differently -- one by symbol id assigned
    at load, one by ticker -- and while the encoding quantizes to a millionth,
    which makes a tie possible, the fixture is checked rather than assumed. A tie
    would put a name on opposite sides of the decile boundary in the two runs and
    produce a parity failure that looked like an accounting bug.
    """
    config = config or ParityConfig()
    frame = bars.copy()
    frame["exposure"] = decode_exposure(frame["volume"].to_numpy())
    dates = sorted(frame["date"].unique())
    for session, date in enumerate(dates):
        if session % config.rebalance_sessions != 0:
            continue
        day = frame.loc[frame["date"] == date, "exposure"]
        if day.duplicated().any():
            raise ValueError(f"exposure ties on rebalance date {date}: the decile boundary "
                             f"would depend on a tiebreak the two implementations do not share")


def write_fixture(out_bars: Path = FIXTURE_BARS, out_equity: Path = FIXTURE_EQUITY,
                  config: ParityConfig | None = None) -> tuple[Path, Path]:
    """Writes the committed fixture and the reference curve beside it."""
    config = config or ParityConfig()
    panel = generate_panel(PARITY_SPEC)
    bars = fixture_bars(panel)
    assert_no_ties(bars, config)

    out_bars.parent.mkdir(parents=True, exist_ok=True)
    formatted = bars.assign(date=bars["date"].dt.strftime("%Y-%m-%d"))
    # Four decimals, matching the repository's other committed sample. Both
    # implementations parse the same text into the same double, so the precision
    # bounds the fixture's realism and not the parity assertion.
    for column in ("open", "high", "low", "close"):
        formatted[column] = formatted[column].map(lambda v: f"{v:.4f}")
    formatted["volume"] = formatted["volume"].round().astype("int64")
    formatted.to_csv(out_bars, index=False,
                     columns=["date", "symbol", "open", "high", "low", "close", "volume"])

    # Simulated from the file, not from the frame that produced it.
    #
    # The CSV carries four decimals and the in-memory panel carries a double, so
    # a curve computed before writing is a curve for prices the engine will never
    # see. It was, at first: sixteen cents of divergence over six months, which
    # would have surfaced as a parity failure with nothing wrong on either side.
    # The expected curve has to come from the same bytes the engine reads.
    run = simulate(pd.read_csv(out_bars, parse_dates=["date"]), config)
    curve = run.to_frame()
    curve.index = curve.index.strftime("%Y-%m-%d")
    curve.index.name = "date"
    curve.to_csv(out_equity)
    return out_bars, out_equity


ROLL_SYMBOL = "ROLL"
"""A name that is present on every session, never departs, and is never traded.

WHY A FIXTURE NEEDS ONE

The engine applies a bar to its book *before* showing it to anybody, so that a
strategy and the fill model pricing its order see the same market. That is the
right rule for a strategy reacting to a print. It is the wrong rule for a
cross-sectional one, because the shim fires `on_session_close(N)` from inside the
dispatch of the **first bar of session N+1** -- so at the instant a session-N
decision is priced, exactly one symbol's book is already a session ahead.

An order in that one symbol therefore fills at session N+1's price while the
decision that sized it used session N's close. That is a real defect in the
engine, it is Session D Part 3's to resolve, and it has nothing to do with
delisting returns. Measured on this fixture before the roll symbol existed: the
short leg's SY0000 order was sized against a close of 148.62 and filled at
143.216, which put the two layers $606.07 apart at session 1 and grew to $942 by
the end.

The churn-free fixture has always been immune to it **by accident**: SY0000 sorts
first on every session and never once lands in either decile. That is not a
property anybody chose, and `assert_roll_symbol_never_trades` now checks it on
both fixtures rather than leaving it to luck.

This fixture makes it deliberate. `ROLL` sorts before every `SY....` ticker, so it
is the first bar of every session and therefore always the symbol the engine
prices a session ahead; its exposure is pinned mid-rank, so it is never in either
decile and no order is ever sent for it. Adding it changes nothing else: the decile
count is `floor(31 * 0.1) == 3`, the same as `floor(30 * 0.1)`, and a name holding
nothing contributes nothing to decision-time equity.
"""

ROLL_PRICE = 100.0
"""Flat, and irrelevant: no order is ever priced against it. Constant rather than
a random walk so that a fixture diff shows only the rows that mean something."""

ROLL_EXPOSURE = 0.0
"""Mid-rank. `assert_no_ties` checks it does not collide with a real one, and
`assert_roll_symbol_never_trades` checks the consequence rather than assuming it."""


def add_roll_symbol(bars: pd.DataFrame) -> pd.DataFrame:
    """Prepends a `ROLL` bar to every session. See `ROLL_SYMBOL`."""
    sessions = pd.Index(sorted(bars["date"].unique()), name="date")
    rows = pd.DataFrame({
        "date": sessions,
        "symbol": ROLL_SYMBOL,
        "open": ROLL_PRICE,
        "high": ROLL_PRICE,
        "low": ROLL_PRICE,
        "close": ROLL_PRICE,
        "volume": float(encode_exposure(ROLL_EXPOSURE)),
    })
    return pd.concat([bars, rows], ignore_index=True).sort_values(
        ["date", "symbol"], kind="stable").reset_index(drop=True)


def assert_roll_symbol_never_trades(bars: pd.DataFrame, config: ParityConfig | None = None,
                                    delisting_returns: dict[str, float] | None = None) -> str:
    """The first symbol of every session is never in a traded leg.

    The property both parity fixtures depend on and neither used to state. If it
    fails, the engine prices that symbol's orders a session ahead of the decision
    that sized them and the two layers part company for a reason that has nothing
    to do with what the test is measuring.

    Returns the symbol it checked, so a caller can report which one carried the
    fixture.
    """
    config = config or ParityConfig()
    first = min(bars["symbol"].unique())
    traded = {symbol for _, symbol, _, _ in
              simulate(bars, config, delisting_returns=delisting_returns).trade_log}
    if first in traded:
        raise ValueError(
            f"{first} sorts first on every session and is traded, so the engine prices its "
            f"orders a session ahead of the decision that sized them. See ROLL_SYMBOL.")

    # And it really is first on every session, not merely first overall.
    for date, day in bars.groupby("date"):
        if min(day["symbol"]) != first:
            raise ValueError(f"{first} is not the first symbol on {date}")
    return str(first)


def churn_membership(bars: pd.DataFrame) -> pd.DataFrame:
    """The membership history the engine needs, read off the bar table.

    `symbol, first_session, last_session`, both inclusive dates. The C++ side
    turns the pair into a `MembershipPeriod` of [midnight of first, midnight after
    last), which covers every bar the name printed and nothing after -- a daily
    bar of date D is timestamped midnight UTC of D, so the session after the last
    one is the first instant the name is not a member, which is when
    `sweep_removals` fires.

    Derived rather than exported from the panel's own `membership` frame on
    purpose: the engine is handed this file and the Python reference is handed the
    bars, so anything that made them disagree would surface as a parity failure
    with nothing wrong on either side. One source, two readers.
    """
    grouped = bars.groupby("symbol")["date"]
    frame = pd.DataFrame({"first_session": grouped.min(), "last_session": grouped.max()})
    frame.index.name = "symbol"
    return frame.sort_index().reset_index()


def assert_no_gaps(bars: pd.DataFrame) -> None:
    """Every name prints on every session between its first and its last.

    A gap is not modelled by either layer and the two would handle one
    differently -- the engine keeps marking a held name from its stale book while
    a membership period that spans the gap says it never left. The generator does
    not produce gaps; this is the assertion that says so rather than the comment
    that assumes it.
    """
    sessions = pd.Index(sorted(bars["date"].unique()))
    for symbol, dates in bars.groupby("symbol")["date"]:
        lo = sessions.get_loc(dates.min())
        hi = sessions.get_loc(dates.max())
        if len(dates) != hi - lo + 1:
            raise ValueError(f"{symbol} has a gap: {len(dates)} bars across "
                             f"{hi - lo + 1} sessions")


def write_churn_fixture(config: ParityConfig | None = None) -> list[Path]:
    """Writes the churn fixture and one reference curve per delisting model.

    Five files: the bars, the membership history, the delisting returns, and the
    expected equity curve under each of the two models. The curves are simulated
    from the written CSV rather than from the frame that produced it, for the same
    reason the churn-free one is -- four decimals on disk against a double in
    memory was sixteen cents of divergence and looked like a parity bug.
    """
    config = config or ParityConfig()
    panel = generate_panel(PARITY_CHURN_SPEC)
    if panel.delistings.empty:
        raise ValueError("the churn spec produced no departures")

    bars = add_roll_symbol(fixture_bars(panel))
    assert_no_gaps(bars)
    assert_no_ties(bars, config)
    assert_roll_symbol_never_trades(
        bars, config, applied_returns(panel.delistings, APPLY_TRUE_RETURN))

    CHURN_BARS.parent.mkdir(parents=True, exist_ok=True)
    formatted = bars.assign(date=bars["date"].dt.strftime("%Y-%m-%d"))
    for column in ("open", "high", "low", "close"):
        formatted[column] = formatted[column].map(lambda v: f"{v:.4f}")
    formatted["volume"] = formatted["volume"].round().astype("int64")
    formatted.to_csv(CHURN_BARS, index=False,
                     columns=["date", "symbol", "open", "high", "low", "close", "volume"])

    membership = churn_membership(bars)
    membership.assign(
        first_session=membership["first_session"].dt.strftime("%Y-%m-%d"),
        last_session=membership["last_session"].dt.strftime("%Y-%m-%d"),
    ).to_csv(CHURN_MEMBERSHIP, index=False)

    # Only the names that actually left. A survivor has no delisting return, and
    # writing one of zero for it would turn "nobody measured this" into "this cost
    # nothing" -- the distinction `DelistingReturns::has` exists to keep.
    delistings = panel.delistings.copy()
    delistings.index.name = "symbol"
    delistings = delistings.reset_index()
    delistings["delisting_return"] = delistings["delisting_return"].map(lambda v: f"{v:.17g}")
    delistings.sort_values("symbol").to_csv(
        CHURN_DELISTINGS, index=False, columns=["symbol", "delisting_return", "cause"])

    written = [CHURN_BARS, CHURN_MEMBERSHIP, CHURN_DELISTINGS]

    from_file = pd.read_csv(CHURN_BARS, parse_dates=["date"])
    held_a_departure = 0
    applied_rows: list[tuple[pd.Timestamp, str, float, float, float, int]] = []
    for model in MODELS:
        run = simulate(from_file, config,
                       delisting_returns=applied_returns(panel.delistings, model))
        held_a_departure = max(held_a_departure, run.forced_exits)
        curve = run.to_frame()
        curve.index = curve.index.strftime("%Y-%m-%d")
        curve.index.name = "date"
        curve.to_csv(CHURN_EQUITY[model])
        written.append(CHURN_EQUITY[model])
        if model == APPLY_TRUE_RETURN:
            applied_rows = run.delisting_log
            if not applied_rows:
                raise ValueError("apply_true_return booked nothing; either no departure was "
                                 "held or the delisting table does not cover the ones that were")

    # Seventeen significant digits on the quantity and the mark, because the C++
    # side is asserted to have computed the identical adjustment from them and a
    # four-decimal mark would make a mismatch unattributable.
    pd.DataFrame(applied_rows,
                 columns=["date", "symbol", "quantity", "mark", "delisting_return",
                          "micros"]).assign(
        date=lambda f: f["date"].dt.strftime("%Y-%m-%d"),
        quantity=lambda f: f["quantity"].map(lambda v: f"{v:.17g}"),
        mark=lambda f: f["mark"].map(lambda v: f"{v:.17g}"),
        delisting_return=lambda f: f["delisting_return"].map(lambda v: f"{v:.17g}"),
    ).to_csv(CHURN_APPLIED, index=False)
    written.append(CHURN_APPLIED)

    # A fixture where no position was ever held into a departure would make churn
    # parity pass without testing anything, which is exactly the failure Session D
    # says not to report as a result.
    if held_a_departure == 0:
        raise ValueError("no position was ever held into a departure; this fixture would "
                         "make churn parity pass by vacuity")
    return written



# The engine's fill model, for predicting what it will charge.
DISPLAYED_AT_TOUCH = 100.0
"""`CsvFeedOptions.quote_size`: what a synthesized quote shows on each side."""

IMPACT_COEFFICIENT_BPS = 10.0
"""`FillModelConfig.impact_coefficient_bps`. Impact is on by default."""


def modelled_cost(bars: pd.DataFrame, config: ParityConfig | None = None) -> dict[str, float]:
    """What the engine should charge for this run, from its own formula.

    The research layer is frictionless and the engine is not, so the two disagree
    by a quantity worth predicting rather than discovering. Crossing the spread
    costs a basis point of what trades; everything beyond the size shown at the
    touch also pays

        impact_bps = impact_coefficient_bps * sqrt(excess / displayed)

    which on orders of a few hundred shares against a hundred displayed is an
    order of magnitude more than the spread. On the committed fixture the spread
    alone comes to $85.84 and the full model to $1,136, against $1,122 measured
    by the engine -- so the gap between the layers is explained, not merely
    observed.
    """
    config = config or ParityConfig()
    run = simulate(bars, config)

    half_spread = config.spread_bps / 10_000.0 / 2.0
    spread_only = 0.0
    with_impact = 0.0
    sizes: list[float] = []

    for _, _symbol, change, price in run.trade_log:
        quantity = abs(float(change))
        if quantity < config.minimum_trade:
            continue
        sizes.append(quantity)
        spread_only += quantity * price * half_spread

        touch = min(quantity, DISPLAYED_AT_TOUCH)
        excess = max(quantity - DISPLAYED_AT_TOUCH, 0.0)
        cost = touch * price * half_spread
        if excess > 0.0:
            impact = IMPACT_COEFFICIENT_BPS * math.sqrt(excess / DISPLAYED_AT_TOUCH) / 1e4
            cost += excess * price * (half_spread + impact)
        with_impact += cost

    return {
        "trades": float(len(sizes)),
        "median_size": float(pd.Series(sizes).median()) if sizes else float("nan"),
        "spread_only": spread_only,
        "spread_plus_impact": with_impact,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="M9 parity fixture and reference run")
    parser.add_argument("--write-fixture", action="store_true")
    args = parser.parse_args(argv)

    panel = generate_panel(PARITY_SPEC)
    bars = fixture_bars(panel)
    assert_no_ties(bars)
    run = simulate(bars)

    start = run.equity.iloc[0] / MICROS
    end = run.equity.iloc[-1] / MICROS
    print(f"{panel.describe()}")
    print(f"reference run: {run.trades} trades, equity {start:,.2f} -> {end:,.2f} "
          f"({(end / (ParityConfig().starting_cash) - 1.0):+.2%})")

    costs = modelled_cost(bars)
    print(f"modelled cost: {int(costs['trades'])} trades, median "
          f"{costs['median_size']:.0f} shares against {DISPLAYED_AT_TOUCH:.0f} displayed -- "
          f"spread ${costs['spread_only']:,.2f}, "
          f"spread+impact ${costs['spread_plus_impact']:,.2f}")

    if args.write_fixture:
        wrote = write_fixture()
        for path in wrote:
            print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
