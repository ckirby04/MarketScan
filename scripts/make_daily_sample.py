#!/usr/bin/env python3
"""Generates the committed synthetic daily dataset.

Tests need a multi-symbol, multi-year daily dataset that is the same on every
machine and never touches the network. Real market data cannot be committed and
a fetched cache cannot be relied on in CI, so this invents one.

It writes CSV rather than .msb. The binary format is what the cache holds and
what the replay reads, but a committed fixture wants to be diffable and
inspectable, and the tests that matter are the ones that convert this to .msb
and back -- which only test anything if the two representations are produced
independently.

Like make_sample_data.py, it reimplements the engine's generator rather than
reaching for `random`, so the file depends on nothing but the seed and does not
change when Python does.

Nothing here is real market data.

    python scripts/make_daily_sample.py
"""

from __future__ import annotations

import argparse
import datetime as dt
import math
from pathlib import Path

MASK64 = (1 << 64) - 1
REPO = Path(__file__).resolve().parent.parent


class DeterministicRng:
    """Mirror of marketscan::DeterministicRng. Same seed, same numbers."""

    def __init__(self, seed: int) -> None:
        self.s = []
        x = seed & MASK64
        for _ in range(4):
            x = (x + 0x9E3779B97F4A7C15) & MASK64
            z = x
            z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
            z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
            self.s.append(z ^ (z >> 31))
        self._spare: float | None = None

    def next_u64(self) -> int:
        s = self.s
        result = (self._rotl((s[1] * 5) & MASK64, 7) * 9) & MASK64
        t = (s[1] << 17) & MASK64
        s[2] ^= s[0]
        s[3] ^= s[1]
        s[1] ^= s[2]
        s[0] ^= s[3]
        s[2] ^= t
        s[3] = self._rotl(s[3], 45)
        return result

    @staticmethod
    def _rotl(x: int, k: int) -> int:
        return ((x << k) | (x >> (64 - k))) & MASK64

    def uniform(self) -> float:
        return (self.next_u64() >> 11) * (1.0 / (1 << 53))

    def normal(self) -> float:
        if self._spare is not None:
            value, self._spare = self._spare, None
            return value
        while True:
            u1 = self.uniform()
            if u1 > 0.0:
                break
        u2 = self.uniform()
        radius = math.sqrt(-2.0 * math.log(u1))
        angle = 2.0 * math.pi * u2
        self._spare = radius * math.sin(angle)
        return radius * math.cos(angle)


def trading_days(start: dt.date, count: int) -> list[dt.date]:
    """Weekdays only. No holiday calendar, and that is a stated simplification:
    a synthetic fixture needs a plausible cadence, not a correct one."""
    days: list[dt.date] = []
    day = start
    while len(days) < count:
        if day.weekday() < 5:
            days.append(day)
        day += dt.timedelta(days=1)
    return days


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=REPO / "data" / "samples")
    parser.add_argument("--days", type=int, default=756, help="trading days (default ~3 years)")
    parser.add_argument("--seed", type=int, default=0x5A17_0DA1_1000_0001)
    args = parser.parse_args()

    # Five names with deliberately different characters, so a cross-sectional
    # strategy sorting on anything has something to sort.
    instruments = [
        # ticker, initial, annual drift, annual vol, mean volume
        ("ALFA", 120.00, 0.10, 0.22, 2_400_000),
        ("BETA", 45.50, 0.04, 0.35, 5_100_000),
        ("GAMM", 310.00, 0.07, 0.18, 900_000),
        ("DLTA", 18.25, -0.02, 0.45, 8_700_000),
        ("EPSL", 76.40, 0.06, 0.28, 1_600_000),
    ]

    rng = DeterministicRng(args.seed)
    dates = trading_days(dt.date(2021, 1, 4), args.days)

    prices = [i[1] for i in instruments]
    dt_year = 1.0 / 252.0
    sqrt_dt = math.sqrt(dt_year)

    rows: list[tuple] = []
    for date in dates:
        for i, (ticker, _, drift, vol, mean_volume) in enumerate(instruments):
            open_px = prices[i]
            shock = vol * sqrt_dt * rng.normal()
            close = open_px * math.exp((drift - 0.5 * vol * vol) * dt_year + shock)
            excursion = abs(rng.normal()) * vol * sqrt_dt * 0.6
            high = max(open_px, close) * (1.0 + excursion)
            low = min(open_px, close) * (1.0 - excursion)
            volume = mean_volume * math.exp(rng.normal() * 0.4)
            prices[i] = close
            rows.append((date, ticker, open_px, high, low, close, round(volume)))

    args.out.mkdir(parents=True, exist_ok=True)
    target = args.out / "synthetic_daily_5.csv"
    with target.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("date,symbol,open,high,low,close,volume\n")
        for date, symbol, o, h, l, c, v in rows:
            handle.write(f"{date},{symbol},{o:.4f},{h:.4f},{l:.4f},{c:.4f},{v}\n")

    print(f"wrote {target} ({len(rows):,} rows, {len(instruments)} symbols, {len(dates)} days)")
    print(f"  {dates[0]} .. {dates[-1]}")
    for i, (ticker, initial, *_rest) in enumerate(instruments):
        print(f"  {ticker}: {initial:.2f} -> {prices[i]:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
