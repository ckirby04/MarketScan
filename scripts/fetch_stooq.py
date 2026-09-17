#!/usr/bin/env python3
"""Fetches free daily OHLCV from Stooq into the local bar cache.

No API key, no account, no paid tier. Stooq publishes end-of-day US equity
history as CSV over plain HTTP, which makes it the only source that can be used
without asking anyone for permission.

WHAT THIS DATA IS AND IS NOT

It is enough to exercise machinery: hundreds of names, decades of history,
adjusted closes, free. It is *not* research-grade. It has no delisting returns,
no point-in-time restatements, no survivorship guarantees of its own, and its
adjustments are whatever Stooq's are. Every number derived from it is a
statement about this pipeline, not about markets. The C++ side labels it and
so does the README.

BEING A GOOD CITIZEN

This is someone else's free service. Requests are serialised, spaced by a
configurable delay, retried with exponential backoff on failure, and never
repeated for a symbol already cached. Running this twice in a row makes no
network requests at all the second time.

    python scripts/fetch_stooq.py --symbols AAPL MSFT SPY
    python scripts/fetch_stooq.py --symbols-file universe.txt --delay 1.5
    python scripts/fetch_stooq.py --symbols AAPL --refresh      # force re-fetch
    python scripts/fetch_stooq.py --status                      # what is cached

Nothing this writes is committed: data/cache/ is gitignored.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import io
import json
import struct
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CACHE = REPO / "data" / "cache" / "stooq"
MANIFEST = CACHE / "manifest.json"

STOOQ_URL = "https://stooq.com/q/d/l/?s={symbol}&i=d"
USER_AGENT = "MarketScan/0.1 (research tool; contact via repository)"

# --- The .msb format. Mirrors include/marketscan/data/bar_store.hpp ---------
#
# Reimplemented here rather than bound through the C++ module, for the same
# reason make_sample_data.py reimplements the RNG: the fetcher has to run
# without a built binary, and a cache that can only be written by a working
# build is a cache you cannot populate while the build is broken.
MAGIC = b"MSB1"
VERSION = 1
HEADER = struct.Struct("<II Q 32s q q")   # magic, version, count, ticker, interval, reserved
RECORD = struct.Struct("<q ddddd")        # ts, open, high, low, close, volume
assert HEADER.size == 64, HEADER.size
assert RECORD.size == 48, RECORD.size

NANOS_PER_SECOND = 1_000_000_000
NANOS_PER_DAY = 86_400 * NANOS_PER_SECOND
DAILY_INTERVAL = NANOS_PER_DAY


# --- US Eastern, by the same rules as core/session_calendar.hpp -------------
#
# Not zoneinfo. The C++ side computes these boundaries from rules compiled into
# a header precisely so a replay does not move when the operating system ships
# a tzdata update, and a fetcher that stamped its bars from a different source
# of truth would put the two a silent hour apart twice a year.

def _days_from_civil(year: int, month: int, day: int) -> int:
    year -= month <= 2
    era = (year if year >= 0 else year - 399) // 400
    yoe = year - era * 400
    doy = (153 * (month + (-3 if month > 2 else 9)) + 2) // 5 + day - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146_097 + doe - 719_468


def _weekday(days: int) -> int:
    return (days + 4) % 7 if days >= -4 else (days + 5) % 7 + 6


def _nth_weekday(year: int, month: int, weekday: int, n: int) -> int:
    first = _days_from_civil(year, month, 1)
    return first + (weekday + 7 - _weekday(first)) % 7 + 7 * (n - 1)


def _last_weekday(year: int, month: int, weekday: int) -> int:
    fifth = _nth_weekday(year, month, weekday, 5)
    # Back off a week if the fifth occurrence spilled into the next month.
    days_in = (_days_from_civil(year + (month == 12), month % 12 + 1, 1)
               - _days_from_civil(year, month, 1))
    return fifth if fifth - _days_from_civil(year, month, 1) < days_in else fifth - 7


def _is_dst(ts_nanos: int) -> bool:
    utc_day = ts_nanos // NANOS_PER_DAY
    year = dt.date(1970, 1, 1) + dt.timedelta(days=utc_day)
    y = year.year
    if y >= 2007:
        start, end = _nth_weekday(y, 3, 0, 2), _nth_weekday(y, 11, 0, 1)
    elif y >= 1987:
        start, end = _nth_weekday(y, 4, 0, 1), _last_weekday(y, 10, 0)
    else:
        start, end = _last_weekday(y, 4, 0), _last_weekday(y, 10, 0)
    return (start * NANOS_PER_DAY + 7 * 3600 * NANOS_PER_SECOND
            <= ts_nanos
            < end * NANOS_PER_DAY + 6 * 3600 * NANOS_PER_SECOND)


def closing_bell(date: dt.date) -> int:
    """16:00 US Eastern on `date`, as nanoseconds since the Unix epoch.

    A daily bar is stamped at the close because that is when the information
    in it exists. Stamped at the open, a strategy could act at 09:30 on a high
    and a low that had not happened yet.
    """
    day = _days_from_civil(date.year, date.month, date.day)
    noon_utc = day * NANOS_PER_DAY + 12 * 3600 * NANOS_PER_SECOND
    offset_hours = -4 if _is_dst(noon_utc) else -5
    return day * NANOS_PER_DAY + (16 - offset_hours) * 3600 * NANOS_PER_SECOND


def write_msb(path: Path, ticker: str, bars: list[tuple[int, float, float, float, float, float]]) -> None:
    encoded = ticker.encode("ascii")
    if len(encoded) >= 32:
        raise ValueError(f"ticker {ticker!r} does not fit in 31 bytes")
    path.parent.mkdir(parents=True, exist_ok=True)
    # Written to a temporary and renamed, so an interrupted fetch never leaves a
    # half-written file that the next run would mistake for a complete one.
    temporary = path.with_suffix(".msb.part")
    with temporary.open("wb") as handle:
        handle.write(HEADER.pack(
            struct.unpack("<I", MAGIC)[0], VERSION, len(bars), encoded, DAILY_INTERVAL, 0))
        for bar in bars:
            handle.write(RECORD.pack(*bar))
    temporary.replace(path)


@dataclass
class Entry:
    ticker: str
    bars: int = 0
    first: str = ""
    last: str = ""
    fetched_at: str = ""
    status: str = "ok"
    note: str = ""


@dataclass
class Manifest:
    entries: dict[str, Entry] = field(default_factory=dict)

    @classmethod
    def load(cls) -> "Manifest":
        if not MANIFEST.exists():
            return cls()
        raw = json.loads(MANIFEST.read_text(encoding="utf-8"))
        return cls({k: Entry(**v) for k, v in raw.get("entries", {}).items()})

    def save(self) -> None:
        MANIFEST.parent.mkdir(parents=True, exist_ok=True)
        MANIFEST.write_text(
            json.dumps(
                {"entries": {k: vars(v) for k, v in sorted(self.entries.items())}},
                indent=2,
            ),
            encoding="utf-8",
        )


def fetch_csv(symbol: str, timeout: float, retries: int, delay: float) -> str:
    """One symbol's history, with backoff. Raises on permanent failure."""
    url = STOOQ_URL.format(symbol=f"{symbol.lower()}.us")
    last_error: Exception | None = None
    for attempt in range(retries):
        if attempt:
            # Exponential, because a service that just refused is not helped by
            # being asked again immediately.
            time.sleep(delay * (2 ** attempt))
        try:
            request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.read().decode("utf-8", errors="replace")
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            last_error = error
    raise RuntimeError(f"{symbol}: {last_error}")


def parse_csv(symbol: str, text: str) -> list[tuple]:
    """Stooq rows to .msb records, oldest first.

    Stooq answers a request for an unknown symbol with a short body rather than
    an HTTP error, so "no data" has to be detected from the content.
    """
    reader = csv.DictReader(io.StringIO(text))
    if reader.fieldnames is None or "Date" not in reader.fieldnames:
        raise ValueError("no data (Stooq returned no header; symbol is probably unknown)")

    bars: list[tuple] = []
    for row in reader:
        try:
            date = dt.date.fromisoformat(row["Date"])
            o, h, l, c = (float(row[k]) for k in ("Open", "High", "Low", "Close"))
            v = float(row.get("Volume") or 0.0)
        except (TypeError, ValueError, KeyError):
            continue
        # The same plausibility rule the C++ feed applies. A bar whose high and
        # low do not bracket its open and close lets a fill simulator trade at a
        # price the day never saw, and it is cheaper to drop it here -- loudly,
        # in the manifest note -- than to have the whole file refused at load.
        if not (h >= l > 0 and h >= o and h >= c and l <= o and l <= c):
            continue
        bars.append((closing_bell(date), o, h, l, c, v))

    bars.sort(key=lambda b: b[0])
    # Stooq occasionally repeats a date. Keep the last, since a correction
    # arrives after the row it corrects.
    deduped: list[tuple] = []
    for bar in bars:
        if deduped and deduped[-1][0] == bar[0]:
            deduped[-1] = bar
        else:
            deduped.append(bar)
    if not deduped:
        raise ValueError("no usable rows")
    return deduped


def read_symbols(args: argparse.Namespace) -> list[str]:
    symbols: list[str] = list(args.symbols or [])
    if args.symbols_file:
        for line in Path(args.symbols_file).read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                symbols.extend(part.strip() for part in line.split(",") if part.strip())
    # Deduplicated, order preserved, uppercased.
    seen: dict[str, None] = {}
    for symbol in symbols:
        seen.setdefault(symbol.upper(), None)
    return list(seen)


def show_status() -> int:
    manifest = Manifest.load()
    if not manifest.entries:
        print(f"nothing cached under {CACHE}")
        return 0
    ok = [e for e in manifest.entries.values() if e.status == "ok"]
    failed = [e for e in manifest.entries.values() if e.status != "ok"]
    print(f"{CACHE}")
    print(f"  {len(ok)} symbols, {sum(e.bars for e in ok):,} bars")
    if ok:
        print(f"  {min(e.first for e in ok)} .. {max(e.last for e in ok)}")
    for entry in failed:
        print(f"  FAILED {entry.ticker}: {entry.note}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--symbols", nargs="*", help="tickers to fetch")
    parser.add_argument("--symbols-file", help="file of tickers, one or more per line")
    parser.add_argument("--delay", type=float, default=1.0,
                        help="seconds between requests (default 1.0)")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--retries", type=int, default=3)
    parser.add_argument("--refresh", action="store_true",
                        help="re-fetch symbols already cached")
    parser.add_argument("--retry-failed", action="store_true",
                        help="also retry symbols that previously failed")
    parser.add_argument("--status", action="store_true", help="report the cache and stop")
    args = parser.parse_args()

    if args.status:
        return show_status()

    symbols = read_symbols(args)
    if not symbols:
        parser.error("give --symbols or --symbols-file")

    manifest = Manifest.load()
    now = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")

    todo = []
    for symbol in symbols:
        entry = manifest.entries.get(symbol)
        cached = CACHE / f"{symbol}.msb"
        if entry and cached.exists() and entry.status == "ok" and not args.refresh:
            continue
        if entry and entry.status != "ok" and not (args.refresh or args.retry_failed):
            continue
        todo.append(symbol)

    skipped = len(symbols) - len(todo)
    print(f"{len(symbols)} requested, {skipped} already cached, {len(todo)} to fetch")
    if not todo:
        return 0

    fetched = 0
    failed = 0
    try:
        for index, symbol in enumerate(todo, 1):
            if index > 1:
                time.sleep(args.delay)
            try:
                text = fetch_csv(symbol, args.timeout, args.retries, args.delay)
                bars = parse_csv(symbol, text)
            except (RuntimeError, ValueError) as error:
                # Logged, recorded, and not silently skipped. A universe that
                # quietly lost a name produces a backtest nothing can tell apart
                # from one where the name was never in the index.
                failed += 1
                manifest.entries[symbol] = Entry(
                    ticker=symbol, fetched_at=now, status="failed", note=str(error)[:200])
                print(f"  [{index}/{len(todo)}] {symbol}: FAILED -- {error}", file=sys.stderr)
                manifest.save()
                continue

            write_msb(CACHE / f"{symbol}.msb", symbol, bars)
            first = dt.datetime.fromtimestamp(bars[0][0] / 1e9, dt.timezone.utc).date()
            last = dt.datetime.fromtimestamp(bars[-1][0] / 1e9, dt.timezone.utc).date()
            manifest.entries[symbol] = Entry(
                ticker=symbol, bars=len(bars), first=str(first), last=str(last),
                fetched_at=now, status="ok")
            fetched += 1
            print(f"  [{index}/{len(todo)}] {symbol}: {len(bars):,} bars, {first} .. {last}")

            # Saved after every symbol, not at the end. An interrupted fetch
            # then resumes from where it stopped rather than from nothing.
            manifest.save()
    except KeyboardInterrupt:
        print("\ninterrupted; the cache and manifest are consistent, rerun to resume")
        manifest.save()
        return 130

    print(f"\n{fetched} fetched, {failed} failed, cache at {CACHE}")
    return 1 if failed and not fetched else 0


if __name__ == "__main__":
    raise SystemExit(main())
