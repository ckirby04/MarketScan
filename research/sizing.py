"""The position sizing rule, in Python. The specification is `docs/SIZING.md`.

    python -m research.sizing --write-cases

This is one of two implementations. The other is
`include/marketscan/strategy/sizing.hpp`, and neither calls the other: the
research layer stays independent of the engine, which is what makes the two
agreeing evidence rather than a tautology.

Every function here is a step of the rule as written, in the order written, with
the types the specification names. Nothing is combined into one expression, even
where that would be shorter, because the reason the two implementations drifted
apart was that the intermediate steps were never named anywhere.
"""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CASES = REPO / "data" / "samples" / "sizing_cases.csv"

MICROS = 1_000_000

MINIMUM_TRADE = 1e-9
"""Below this many shares a delta is not submitted. See SIZING.md step 7."""


def notional_micros(price: float, quantity: float) -> int:
    """`Money::notional`: micro-dollars, rounded half away from zero.

    Python's `round` is banker's rounding and `np.round` likewise; `std::llround`
    is half away from zero. On an exact half they differ by one micro-dollar,
    which is one more than an exact parity assertion tolerates.
    """
    micros = price * quantity * MICROS
    if not math.isfinite(micros):
        raise ValueError(f"non-finite notional from price={price} quantity={quantity}")
    return int(math.copysign(math.floor(abs(micros) + 0.5), micros))


def side_count(count: int, decile: float) -> int:
    """Step 1: how many names each leg takes. Zero means stand down."""
    if count <= 0 or decile <= 0.0:
        return 0
    side = int(math.floor(count * decile))
    if side == 0 or side * 2 > count:
        return 0
    return side


def decision_equity(cash_micros: int, marks: dict[str, float],
                    positions: dict[str, float]) -> int:
    """Step 2: cash plus every position marked at **this session's** close.

    Marked from the cross-section the decision is based on, not from whatever the
    engine's book happens to hold. That distinction is the whole of SIZING.md's
    second section: the engine marks one symbol per session at the *next*
    session's close, and sizing from that is both a disagreement between the
    layers and a small lookahead.
    """
    total = cash_micros
    for symbol, quantity in positions.items():
        if quantity == 0.0:
            continue
        mark = marks.get(symbol)
        if mark is None:
            continue
        total += notional_micros(mark, quantity)
    return total


def per_name_notional(equity_micros: int, gross: float, side: int) -> float:
    """Steps 3 and 4: the dollar target for one name on one leg."""
    if side <= 0:
        return 0.0
    book = (equity_micros / MICROS) * gross
    return book / (2 * side)


def target_shares(per_name: float, close: float, long_side: bool) -> float:
    """Step 5. Fractional by design -- SIZING.md says why."""
    if close <= 0.0:
        raise ValueError(f"a target cannot be sized against a close of {close}")
    return (per_name if long_side else -per_name) / close


def trade_delta(target: float, position: float) -> float:
    """Step 6."""
    return target - position


def should_submit(delta: float) -> bool:
    """Step 7."""
    return abs(delta) >= MINIMUM_TRADE


# ---------------------------------------------------------------------------
# The shared cross-test grid
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Case:
    """One row of the grid both suites read."""

    name: str
    equity_micros: int
    gross: float
    count: int
    decile: float
    close: float
    position: float
    long_side: bool

    def evaluate(self) -> dict[str, object]:
        side = side_count(self.count, self.decile)
        per_name = per_name_notional(self.equity_micros, self.gross, side)
        if side == 0:
            return {"side": 0, "per_name": 0.0, "target": 0.0, "delta": 0.0, "submit": 0}
        target = target_shares(per_name, self.close, self.long_side)
        delta = trade_delta(target, self.position)
        return {
            "side": side,
            "per_name": per_name,
            "target": target,
            "delta": delta,
            "submit": 1 if should_submit(delta) else 0,
        }


def grid() -> list[Case]:
    """The cases, including the ones chosen to be awkward.

    Adversarial on purpose, per SIZING.md: prices near zero, equity at rounding
    boundaries, deciles that do not divide cleanly, cross-sections too small to
    form two legs, and the configuration that produced the 84-session divergence
    in the M9 fixture.
    """
    cases: list[Case] = []

    # The ordinary middle of the range.
    for equity in (100_000 * MICROS, 106_660_549_447, 1 * MICROS):
        for gross in (1.0, 0.5, 2.0):
            for close in (39.5234, 250.0, 1.0):
                cases.append(Case(f"plain/e{equity}/g{gross:g}/p{close:g}",
                                  equity, gross, 30, 0.1, close, 0.0, True))

    # The fixture's own shape, long and short, flat and already-held.
    for position in (0.0, 306.41724137931033, -306.41724137931033):
        for long_side in (True, False):
            cases.append(Case(f"fixture/pos{position:g}/{'long' if long_side else 'short'}",
                              100_000 * MICROS, 1.0, 30, 0.1, 39.5234, position, long_side))

    # Deciles that do not divide cleanly, and cross-sections at the boundary.
    for count, decile in ((30, 0.1), (31, 0.1), (7, 0.1), (10, 0.5), (9, 0.5),
                          (2, 0.5), (1, 0.1), (0, 0.1), (100, 0.03)):
        cases.append(Case(f"boundary/n{count}/d{decile:g}",
                          100_000 * MICROS, 1.0, count, decile, 50.0, 0.0, True))

    # Prices near zero: a penny stock is a real thing and a divide is a divide.
    for close in (0.01, 0.0001, 1e-6):
        cases.append(Case(f"cheap/p{close:g}", 100_000 * MICROS, 1.0, 30, 0.1,
                          close, 0.0, True))

    # Equity at and around a micro-dollar rounding boundary.
    for equity in (1, 5, 500_000, 999_999, 1_000_001, 2_500_000):
        cases.append(Case(f"boundary/e{equity}", equity, 1.0, 30, 0.1, 39.5234, 0.0, True))

    # A delta that lands exactly on the minimum-trade threshold, and either side.
    target = target_shares(per_name_notional(100_000 * MICROS, 1.0, 3), 39.5234, True)
    for offset, label in ((0.0, "zero"), (MINIMUM_TRADE, "at"), (MINIMUM_TRADE / 2, "under")):
        cases.append(Case(f"minimum/{label}", 100_000 * MICROS, 1.0, 30, 0.1, 39.5234,
                          target - offset, True))

    return cases


def write_cases(path: Path = CASES) -> Path:
    """Writes the grid with its expected outputs, round-trip exact.

    `%.17g` because that is the shortest decimal form guaranteed to recover a
    binary64 exactly. The two suites assert bit equality against these numbers,
    so anything less would be asserting that the CSV writer and the parser agree
    rather than that the implementations do.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        writer = csv.writer(handle)
        writer.writerow(["name", "equity_micros", "gross", "count", "decile", "close",
                         "position", "long_side", "side", "per_name", "target", "delta",
                         "submit"])
        for case in grid():
            out = case.evaluate()
            writer.writerow([
                case.name, case.equity_micros, f"{case.gross:.17g}", case.count,
                f"{case.decile:.17g}", f"{case.close:.17g}", f"{case.position:.17g}",
                1 if case.long_side else 0, out["side"], f"{out['per_name']:.17g}",
                f"{out['target']:.17g}", f"{out['delta']:.17g}", out["submit"],
            ])
    return path


def read_cases(path: Path = CASES) -> list[tuple[Case, dict[str, object]]]:
    """The committed grid, as (inputs, expected outputs)."""
    rows: list[tuple[Case, dict[str, object]]] = []
    with Path(path).open(encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            case = Case(
                name=row["name"],
                equity_micros=int(row["equity_micros"]),
                gross=float(row["gross"]),
                count=int(row["count"]),
                decile=float(row["decile"]),
                close=float(row["close"]),
                position=float(row["position"]),
                long_side=row["long_side"] == "1",
            )
            expected = {
                "side": int(row["side"]),
                "per_name": float(row["per_name"]),
                "target": float(row["target"]),
                "delta": float(row["delta"]),
                "submit": int(row["submit"]),
            }
            rows.append((case, expected))
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--write-cases", action="store_true")
    args = parser.parse_args(argv)

    cases = grid()
    print(f"{len(cases)} sizing cases")
    if args.write_cases:
        print(f"wrote {write_cases()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
