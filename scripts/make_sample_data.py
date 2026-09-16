#!/usr/bin/env python3
"""Regenerate the committed sample datasets under data/samples/.

The output is checked in, so this script exists to document how the data was
made and to let anyone reproduce it byte for byte -- not because the build
needs to run it.

It reimplements the same generator the C++ SyntheticFeed uses (SplitMix64 seed
expansion, xoshiro256** stream, Box-Muller normals) rather than reaching for
`random`, so the file it writes depends on nothing but the seed. A sample
dataset that changed when Python was upgraded would quietly change every test
that reads it.

Nothing here is real market data.

Usage:
    python scripts/make_sample_data.py
"""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

MASK64 = (1 << 64) - 1


class DeterministicRng:
    """Mirror of marketscan::DeterministicRng. Same seed, same numbers."""

    def __init__(self, seed: int) -> None:
        self._state = [0, 0, 0, 0]
        self._spare: float | None = None
        x = seed & MASK64
        for i in range(4):
            x = (x + 0x9E3779B97F4A7C15) & MASK64
            z = x
            z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
            z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
            self._state[i] = z ^ (z >> 31)

    @staticmethod
    def _rotl(x: int, k: int) -> int:
        return ((x << k) | (x >> (64 - k))) & MASK64

    def next_u64(self) -> int:
        s = self._state
        result = (self._rotl((s[1] * 5) & MASK64, 7) * 9) & MASK64
        t = (s[1] << 17) & MASK64
        s[2] ^= s[0]
        s[3] ^= s[1]
        s[1] ^= s[2]
        s[0] ^= s[3]
        s[2] ^= t
        s[3] = self._rotl(s[3], 45)
        return result

    def uniform(self) -> float:
        return (self.next_u64() >> 11) * (2.0**-53)

    def uniform_positive(self) -> float:
        return ((self.next_u64() >> 11) + 1.0) * (2.0**-53)

    def normal(self) -> float:
        if self._spare is not None:
            value, self._spare = self._spare, None
            return value
        u1 = self.uniform_positive()
        u2 = self.uniform()
        magnitude = math.sqrt(-2.0 * math.log(u1))
        angle = 2.0 * math.pi * u2
        self._spare = magnitude * math.sin(angle)
        return magnitude * math.cos(angle)


@dataclass(frozen=True)
class Instrument:
    ticker: str
    initial_price: float
    annual_drift: float
    annual_volatility: float
    mean_volume: float


def generate_bars(
    instruments: list[Instrument],
    start: datetime,
    sessions: int,
    bars_per_session: int,
    seed: int,
    trading_days_per_year: float = 252.0,
    intrabar_range_factor: float = 0.6,
) -> list[tuple[datetime, str, float, float, float, float, float]]:
    rng = DeterministicRng(seed)
    prices = [i.initial_price for i in instruments]
    dt = 1.0 / (trading_days_per_year * bars_per_session)
    sqrt_dt = math.sqrt(dt)

    rows = []
    for index in range(sessions * bars_per_session):
        session, within = divmod(index, bars_per_session)
        ts = start + timedelta(days=session) + timedelta(minutes=within)

        for i, instrument in enumerate(instruments):
            open_px = prices[i]
            drift = (
                instrument.annual_drift
                - 0.5 * instrument.annual_volatility**2
            ) * dt
            shock = instrument.annual_volatility * sqrt_dt * rng.normal()
            close = open_px * math.exp(drift + shock)

            body_high = max(open_px, close)
            body_low = min(open_px, close)
            excursion = (
                instrument.annual_volatility * sqrt_dt * intrabar_range_factor
            )
            high = body_high * (1.0 + excursion * abs(rng.normal()))
            low = body_low * (1.0 - excursion * abs(rng.normal()))
            volume = instrument.mean_volume * math.exp(rng.normal() * 0.35)

            prices[i] = close
            rows.append(
                (ts, instrument.ticker, open_px, high, low, close, round(volume))
            )
    return rows


def write_csv(path: Path, rows) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            "# Synthetic data. Not real market data, not from any vendor.\n"
            "# Regenerate with: python scripts/make_sample_data.py\n"
        )
        handle.write("timestamp,symbol,open,high,low,close,volume\n")
        for ts, symbol, o, h, l, c, v in rows:
            handle.write(
                f"{ts.strftime('%Y-%m-%dT%H:%M:%SZ')},{symbol},"
                f"{o:.4f},{h:.4f},{l:.4f},{c:.4f},{v:.0f}\n"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "data" / "samples",
        help="directory to write into",
    )
    args = parser.parse_args()

    # A US equity session is 390 one-minute bars, 13:30-19:59 UTC while the
    # east coast is on daylight time.
    start = datetime(2024, 3, 18, 13, 30, tzinfo=timezone.utc)

    rows = generate_bars(
        instruments=[
            Instrument("SPY", 510.00, 0.08, 0.15, 90_000.0),
        ],
        start=start,
        sessions=2,
        bars_per_session=390,
        seed=0xC0FFEE0012345678,
    )
    target = args.out / "synthetic_spy_1min.csv"
    write_csv(target, rows)
    closes = [r[5] for r in rows]
    print(f"wrote {target} ({len(rows)} bars)")
    print(f"  close: {closes[0]:.2f} -> {closes[-1]:.2f}")
    print(f"  range: {min(r[4] for r in rows):.2f} - {max(r[3] for r in rows):.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
