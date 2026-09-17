#!/usr/bin/env python3
"""Converts a CSV of daily bars into the binary cache format.

Two uses. Someone with daily data from anywhere -- a broker export, a vendor
file, the committed synthetic fixture -- can put it in the cache without
involving the fetcher. And the round trip CSV -> .msb -> replay is how the
committed fixture gets exercised through the real cache path rather than
through a test-only shortcut.

The writer is imported from fetch_stooq.py rather than reimplemented, because
two copies of a binary layout is one copy too many.

    python scripts/csv_to_msb.py data/samples/synthetic_daily_5.csv --out data/cache/sample
    python scripts/csv_to_msb.py export.csv --out data/cache/mine --symbol AAPL
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import importlib.util
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def load_writer():
    """fetch_stooq's .msb writer and timestamp convention.

    Imported by path and registered in sys.modules first: a module executed
    without being registered has no entry for dataclasses to look its
    annotations up in, and the decorator fails with an error that says nothing
    about the real cause.
    """
    spec = importlib.util.spec_from_file_location("marketscan_fetch", REPO / "scripts" / "fetch_stooq.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


# Header spellings a real export actually uses. An unrecognised column is
# ignored; a missing required one is an error.
ALIASES = {
    "date": "date", "timestamp": "date", "ts": "date", "datetime": "date", "time": "date",
    "symbol": "symbol", "ticker": "symbol",
    "open": "open", "o": "open",
    "high": "high", "h": "high",
    "low": "low", "l": "low",
    "close": "close", "c": "close", "adj close": "close", "adj_close": "close",
    "volume": "volume", "v": "volume", "vol": "volume",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path)
    parser.add_argument("--out", type=Path, required=True, help="cache directory to write into")
    parser.add_argument("--symbol", help="ticker for a file with no symbol column")
    args = parser.parse_args()

    fetch = load_writer()

    with args.source.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise SystemExit(f"{args.source}: no header")
        columns = {ALIASES.get(name.strip().lower()): name
                   for name in reader.fieldnames
                   if ALIASES.get(name.strip().lower())}

        for required in ("date", "open", "high", "low", "close"):
            if required not in columns:
                raise SystemExit(f"{args.source}: no {required} column")
        if "symbol" not in columns and not args.symbol:
            raise SystemExit(
                f"{args.source}: no symbol column and no --symbol. Refusing to guess which "
                "instrument this is.")

        series: dict[str, list[tuple]] = {}
        for line, row in enumerate(reader, 2):
            ticker = (row[columns["symbol"]] if "symbol" in columns else args.symbol).strip().upper()
            try:
                date = dt.date.fromisoformat(row[columns["date"]].strip()[:10])
                o, h, l, c = (float(row[columns[k]]) for k in ("open", "high", "low", "close"))
                v = float(row[columns["volume"]]) if "volume" in columns else 0.0
            except (ValueError, KeyError, AttributeError) as error:
                raise SystemExit(f"{args.source}:{line}: {error}") from error

            if not (h >= l > 0 and h >= o and h >= c and l <= o and l <= c):
                raise SystemExit(
                    f"{args.source}:{line}: impossible bar for {ticker} -- the high and low do "
                    "not bracket the open and close, which would let a fill simulator trade at a "
                    "price the day never saw")
            series.setdefault(ticker, []).append((fetch.closing_bell(date), o, h, l, c, v))

    if not series:
        raise SystemExit(f"{args.source}: no rows")

    args.out.mkdir(parents=True, exist_ok=True)
    total = 0
    for ticker in sorted(series):
        bars = sorted(series[ticker], key=lambda b: b[0])
        fetch.write_msb(args.out / f"{ticker}.msb", ticker, bars)
        total += len(bars)
        print(f"  {ticker}: {len(bars):,} bars")

    print(f"wrote {len(series)} files, {total:,} bars, to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
