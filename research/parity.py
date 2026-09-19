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
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd

from research.panel import Panel, PanelSpec, generate_panel

REPO = Path(__file__).resolve().parent.parent
FIXTURE_BARS = REPO / "data" / "samples" / "parity_panel.csv"
FIXTURE_EQUITY = REPO / "data" / "samples" / "parity_expected.csv"

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


def encode_exposure(exposure: float | np.ndarray) -> np.ndarray:
    return np.round((np.asarray(exposure, dtype=float) + EXPOSURE_OFFSET) * EXPOSURE_SCALE)


def decode_exposure(volume: float | np.ndarray) -> np.ndarray:
    return np.asarray(volume, dtype=float) / EXPOSURE_SCALE - EXPOSURE_OFFSET


def notional(price: float, quantity: float) -> int:
    """Micro-dollars, rounded half away from zero. `Money::notional`, in Python.

    `np.round` is banker's rounding and would disagree with `std::llround` on
    every exact half. The difference is one micro-dollar, which is one more than
    a parity test asserting exactness can tolerate.
    """
    micros = price * quantity * MICROS
    if not math.isfinite(micros):
        raise ValueError(f"non-finite notional from price={price} quantity={quantity}")
    return int(math.copysign(math.floor(abs(micros) + 0.5), micros))


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
    trades: int
    commission: int

    def to_frame(self) -> pd.DataFrame:
        return pd.DataFrame({"equity_micros": self.equity.astype("int64"),
                             "cash_micros": self.cash.astype("int64")})


def _fill_price(close: float, buying: bool, config: ParityConfig) -> float:
    if config.optimistic:
        return close
    half = close * config.spread_bps / 10_000.0 / 2.0
    return close + half if buying else close - half


def simulate(bars: pd.DataFrame, config: ParityConfig | None = None) -> ParityRun:
    """Runs the decile rule over a bar table, in the engine's accounting.

    `bars` carries date, symbol, close and volume, with the exposure encoded in
    volume. Everything here mirrors `decile_parity.hpp` step for step: the same
    sort, the same decile boundary, closes before opens, the same epsilon on a
    trade too small to bother with.
    """
    config = config or ParityConfig()

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

    for session, date in enumerate(closes.index):
        close_row = closes.loc[date].dropna()
        exposure_row = exposures.loc[date].reindex(close_row.index)

        if session % config.rebalance_sessions == 0 and not close_row.empty:
            # Descending by exposure, ties broken by symbol so that the decile
            # boundary cannot depend on an ordering the two implementations do
            # not share. `assert_no_ties` checks that the tiebreak never
            # actually has to arbitrate.
            order = sorted(close_row.index,
                           key=lambda symbol: (-exposure_row[symbol], symbol))
            count = len(order)
            side = int(math.floor(count * config.decile))

            if side > 0 and side * 2 <= count:
                equity_now = cash + sum(
                    notional(float(close_row[symbol]), quantity)
                    for symbol, quantity in shares.items() if symbol in close_row.index)
                per_name = (equity_now / MICROS) * config.gross / (2 * side)

                targets = {symbol: 0.0 for symbol in order[side:count - side]}
                targets.update({symbol: +per_name for symbol in order[:side]})
                targets.update({symbol: -per_name for symbol in order[count - side:]})

                # Closes first, then opens -- the same order as the theory, so
                # that a cash-constrained rebalance is constrained identically.
                ordered = ([s for s in order[side:count - side]] +
                           [s for s in order[:side]] + [s for s in order[count - side:]])
                for symbol in ordered:
                    price = float(close_row[symbol])
                    target_shares = targets[symbol] / price if targets[symbol] else 0.0
                    delta = target_shares - shares.get(symbol, 0.0)
                    if abs(delta) < config.minimum_trade:
                        continue
                    fill = _fill_price(price, buying=delta > 0, config=config)
                    cash -= notional(fill, delta)
                    charge = notional(config.commission_per_share, abs(delta))
                    cash -= charge
                    commission_paid += charge
                    shares[symbol] = shares.get(symbol, 0.0) + delta
                    if abs(shares[symbol]) < config.minimum_trade:
                        shares.pop(symbol)
                    trades += 1

        marked = sum(notional(float(close_row[symbol]), quantity)
                     for symbol, quantity in shares.items() if symbol in close_row.index)
        equity_by_date[date] = cash + marked
        cash_by_date[date] = cash
        position_rows[date] = dict(shares)

    return ParityRun(
        equity=pd.Series(equity_by_date, dtype="int64").sort_index(),
        cash=pd.Series(cash_by_date, dtype="int64").sort_index(),
        positions=pd.DataFrame(position_rows).T.sort_index().fillna(0.0),
        trades=trades,
        commission=commission_paid,
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
    run = simulate(bars, ParityConfig(optimistic=True, **{
        k: v for k, v in vars(config).items() if k != "optimistic"}))
    closes = bars.pivot(index="date", columns="symbol", values="close")

    half_spread = config.spread_bps / 10_000.0 / 2.0
    spread_only = 0.0
    with_impact = 0.0
    sizes: list[float] = []

    previous = None
    for date, row in run.positions.iterrows():
        delta = row - previous if previous is not None else row
        previous = row
        for symbol, change in delta.items():
            quantity = abs(float(change))
            if quantity < config.minimum_trade:
                continue
            price = float(closes.loc[date, symbol])
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
