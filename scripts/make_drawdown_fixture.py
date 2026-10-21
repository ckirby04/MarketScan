"""Writes the drawdown_entry cross-test fixtures.

    python scripts/make_drawdown_fixture.py

Produces, under data/samples/research/:

  drawdown_prices.csv     a synthetic daily close series, seeded
  drawdown_expected.json  every fill the research layer's implementation
                          (research/drawdown_entry.py) makes on it, per case in
                          drawdown_cases.json

The engine's implementation is tested against drawdown_expected.json by
tests/test_research_rule.cpp, and the research layer's is tested against it by
research/tests/test_drawdown_entry.py, which also regenerates it and requires the
committed file to match. So a change to either implementation that moves a fill
fails a test on one side or the other.

The path is not a market. It is built to visit every branch of the rule: a long
rise to set highs, a crash deep enough for every tranche, a recovery past the old
high, choppy stretches for the trailing and time stops, and isolated gap-ups that
make an unreserved buy unaffordable at the fill.
"""

from __future__ import annotations

import csv
import datetime as dt
import json
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from research import drawdown_entry as rule  # noqa: E402

OUT = REPO / "data" / "samples" / "research"
SEED = 20261021


def business_days(start: dt.date, n: int) -> list[dt.date]:
    days = []
    d = start
    while len(days) < n:
        if d.weekday() < 5:
            days.append(d)
        d += dt.timedelta(days=1)
    return days


def make_path() -> tuple[list[dt.date], list[float]]:
    rng = np.random.default_rng(SEED)
    segments = [
        (250, 0.0008, 0.012),   # a rise that sets the highs
        (120, -0.0060, 0.025),  # the crash
        (200, 0.0045, 0.018),   # recovery past the old high
        (150, 0.0000, 0.022),   # chop
        (100, -0.0030, 0.020),  # a second, shallower fall
        (130, 0.0030, 0.015),   # and out of it
    ]
    returns = np.concatenate([rng.normal(mu, sigma, n) for n, mu, sigma in segments])
    # Isolated overnight gap-ups, for step 7's refusal.
    for index in rng.choice(np.arange(20, returns.size), size=25, replace=False):
        returns[index] = 0.05
    levels = 100.0 * np.exp(np.cumsum(returns))
    closes = [round(float(x), 4) for x in levels]
    return business_days(dt.date(2000, 1, 3), len(closes)), closes


def load_prices() -> tuple[list[dt.date], list[float]]:
    dates, closes = [], []
    with (OUT / "drawdown_prices.csv").open(newline="") as f:
        for row in csv.DictReader(f):
            dates.append(dt.date.fromisoformat(row["date"]))
            closes.append(float(row["close"]))
    return dates, closes


def expected(dates, closes) -> dict:
    corpus = json.loads((OUT / "drawdown_cases.json").read_text(encoding="utf-8"))
    out = {}
    for case in corpus["cases"]:
        costs = rule.Costs(
            starting_cash=case["starting_cash"] * rule.MICROS,
            spread_bps=case["spread_bps"],
            commission=case["commission"],
            reserve=case["reserve"],
        )
        start = rule.day_number(dt.date.fromisoformat(case["start"]))
        result = rule.simulate(dates, closes, rule.params_from_case(case), costs, start)
        out[case["name"]] = {
            "rejected": result.rejected,
            "fills": [
                {"bar": f.bar, "side": f.side, "quantity": f.quantity, "price": f.price,
                 "tag": f.tag}
                for f in result.fills
            ],
        }
    return out


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    dates, closes = make_path()
    with (OUT / "drawdown_prices.csv").open("w", newline="") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(["date", "close"])
        for d, c in zip(dates, closes):
            writer.writerow([d.isoformat(), repr(c)])
    dates, closes = load_prices()  # exactly what the tests will read
    (OUT / "drawdown_expected.json").write_text(
        json.dumps(expected(dates, closes), indent=1) + "\n", encoding="utf-8", newline="\n"
    )
    print(f"wrote {len(closes)} bars and the expected fills to {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
