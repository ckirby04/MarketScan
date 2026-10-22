#!/usr/bin/env python3
"""Writes data/samples/research/synthetic_cases.json from research/synthetic.py.

    python scripts/make_synthetic_cases.py

The cross-test fixture for JOB.md section 1a: inputs made from a seeded random
walk -- no market data, licensed or otherwise -- and the synthetic closes the
research layer computes for them. tests/test_research_job.cpp holds the engine's
extend_history to these exactly, and research/tests/test_synthetic.py holds
synthetic.py to them, so neither implementation can drift alone.
"""

from __future__ import annotations

import datetime as dt
import json
import random
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from research import synthetic  # noqa: E402

OUT = REPO / "data" / "samples" / "research" / "synthetic_cases.json"


def weekdays(start: dt.date, n: int) -> list[str]:
    days, day = [], start
    while len(days) < n:
        if day.weekday() < 5:
            days.append(day.isoformat())
        day += dt.timedelta(days=1)
    return days


def case(name: str, seed: int, n: int, anchor: int, rate_from: int, params: dict,
         daily_vol: float = 0.015, drop_anchor: bool = False) -> dict:
    rng = random.Random(seed)
    days = weekdays(dt.date(1999, 3, 1), n)
    closes, price = [], 100.0
    for _ in days:
        price *= 1.0 + rng.gauss(0.0004, daily_vol)
        closes.append(round(price, 4))
    # Rates on a sparser calendar than the prices, starting late and with gaps,
    # so "on or before" is exercised.
    rate_days = [d for i, d in enumerate(days) if i >= rate_from and i % 3 != 1]
    rates = [round(max(0.0, 4.0 + rng.gauss(0.0, 1.5)), 2) for _ in rate_days]
    first_real_day = days[anchor]
    underlying_days, underlying = days, closes
    if drop_anchor:
        underlying_days = days[:anchor] + days[anchor + 1:]
        underlying = closes[:anchor] + closes[anchor + 1:]
    inputs = {
        "underlying_days": underlying_days, "underlying": underlying,
        "rate_days": rate_days, "rates": rates,
        "first_real_day": first_real_day, "first_real_close": 50.0,
        "params": params,
    }
    p = synthetic.Params(params["leverage"], params["annual_fee_pct"], params["borrow_spread_pct"])
    try:
        out_days, out_closes = synthetic.extend(underlying_days, underlying, rate_days, rates,
                                                first_real_day, 50.0, p)
        expected = {"days": out_days, "closes": out_closes}
    except synthetic.RecipeError as error:
        expected = {"error": str(error)}
    return {"name": name, "inputs": inputs, "expected": expected}


def main() -> int:
    three = {"leverage": 3, "annual_fee_pct": 0.95, "borrow_spread_pct": 0.5}
    cases = [
        case("3x, rates from the start", 1, 260, 200, 0, three),
        case("3x, rates start late: history stops at the first rate", 2, 260, 200, 40, three),
        case("-1x inverse", 3, 120, 100, 0, {"leverage": -1, "annual_fee_pct": 0.9, "borrow_spread_pct": 0.0}),
        case("2x with no fee and no spread", 4, 80, 79, 0, {"leverage": 2, "annual_fee_pct": 0, "borrow_spread_pct": 0}),
        case("the underlying has no bar on the first real day", 5, 60, 30, 0, three, drop_anchor=True),
        case("a wipeout: the recipe takes the price to zero", 6, 120, 100, 0,
             {"leverage": 10, "annual_fee_pct": 0.95, "borrow_spread_pct": 0.5}, daily_vol=0.12),
    ]
    OUT.write_text(json.dumps({
        "about": "JOB.md section 1a cross-test cases, written by scripts/make_synthetic_cases.py "
                 "from research/synthetic.py. Seeded random walks; no market data.",
        "cases": cases,
    }, indent=1) + "\n", encoding="utf-8")
    for c in cases:
        e = c["expected"]
        print(f"{c['name']}: " + (f"{len(e['days'])} synthetic bars" if "days" in e else f"error: {e['error']}"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
