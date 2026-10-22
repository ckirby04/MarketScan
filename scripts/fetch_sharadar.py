#!/usr/bin/env python3
"""Fetches daily prices from Sharadar into a .msb cache outside the repository.

    $env:SHARADAR_API_KEY = "..."
    python scripts/fetch_sharadar.py --tickers AAPL MSFT
    python scripts/fetch_sharadar.py --tickers-file universe.txt --refresh
    python scripts/fetch_sharadar.py --status

Sharadar (https://sharadar.com) is an approved source under docs/DATA_POLICY.md
section 5: an API key, documented personal-use terms (https://sharadar.com/terms),
and delisted securities in the same tables as listed ones. Its data is
licensed, which decides where it goes and how it is asked for:

  * The key comes from SHARADAR_API_KEY and nowhere else (DATA_POLICY section 7's
    rule, applied to every credential). Absent, the script stops and says so.
    It is sent as a header, never in a URL, and never printed.
  * The cache lives OUTSIDE the repository -- `--out`, by default
    ../marketscan-data/sharadar beside the repository -- and a path inside the
    repository is refused. A gitignored folder is one `git add -f` from a
    licence breach (DATA_POLICY section 4).
  * Requests are sliced by ticker and date, never by paging a whole table, one
    at a time with a delay, and a rate limit is backed off from (section 2).

What is written, per ticker, under --out:

  SYMBOL.msb   split-adjusted daily OHLCV, Sharadar's `close`: the form's
               `price_adjustment: split_only`, which is what the job honours
  SYMBOL.TR.msb  the same bars scaled to Sharadar's `closeadj` (splits,
               dividends, spinoffs): a total-return series, which is what a
               synthetic recipe compounds its underlying from (CATALOG.md)
  csv/SYMBOL.csv  every column as delivered, including `closeadj` (splits,
               dividends, spinoffs) and `closeunadj`, for the catalog and for
               total return later
  manifest.json  what each ticker is (table, permaticker, listed or delisted,
               first and last price date) and what happened fetching it

A ticker that names more than one security in Sharadar's `tickers` table is
refused rather than guessed at: a ticker is not an identity (docs/IDENTITY.md),
and prices fetched by ticker for a reused one would weld two companies together.
A ticker outside the subscription is recorded as failed with the vendor's reason,
never silently skipped.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import importlib.util
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
API = "https://api.sharadar.com/v1.0"
KEY_VARIABLE = "SHARADAR_API_KEY"
DEFAULT_OUT = REPO.parent / "marketscan-data" / "sharadar"
FIELDS = "date,open,high,low,close,volume,closeadj,closeunadj"
PAGE = 10_000
USER_AGENT = "marketscan-fetch-sharadar/0.1"


class FetchError(RuntimeError):
    pass


def load_writer():
    """fetch_stooq's .msb writer and closing-bell stamp, so there is one copy."""
    spec = importlib.util.spec_from_file_location(
        "marketscan_fetch", REPO / "scripts" / "fetch_stooq.py"
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def api_key() -> str:
    key = os.environ.get(KEY_VARIABLE, "").strip()
    if not key:
        raise FetchError(f"{KEY_VARIABLE} is not set. The key comes from the environment and nowhere else.")
    return key


def check_out_dir(out: Path) -> Path:
    resolved = out.resolve()
    if resolved == REPO or REPO in resolved.parents:
        raise FetchError(
            f"{out} is inside the repository. Licensed data lives outside it "
            "(docs/DATA_POLICY.md section 4); pass --out somewhere else."
        )
    return resolved


def tls_context() -> ssl.SSLContext:
    """Certificate verification, always -- against certifi's bundle when it is installed.

    Python on Windows builds chains from the Windows certificate store, which can
    hold a root or cross-sign that has since expired; the vendor's chain then
    fails as "certificate has expired" while browsers and curl, which repair the
    chain themselves, succeed. certifi's bundle is current. Verification is never
    turned off: a fetch that cannot tell who it is talking to has not fetched.
    """
    try:
        import certifi
    except ImportError:
        return ssl.create_default_context()
    return ssl.create_default_context(cafile=certifi.where())


TLS = tls_context()


def get_json(path: str, params: dict, key: str, delay: float, retries: int = 4) -> dict:
    """One request, with backoff on a rate limit or a server error."""
    url = f"{API}/{path}?{urllib.parse.urlencode({**params, 'format': 'json'})}"
    request = urllib.request.Request(url, headers={"x-api-key": key, "User-Agent": USER_AGENT})
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(request, timeout=60, context=TLS) as response:
                return json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            body = error.read().decode("utf-8", errors="replace")
            try:
                detail = json.loads(body).get("error", body)
            except json.JSONDecodeError:
                detail = body[:200]
            if error.code in (429, 500, 502, 503, 504) and attempt + 1 < retries:
                wait = float(error.headers.get("Retry-After") or delay * 2 ** (attempt + 1))
                time.sleep(wait)
                continue
            raise FetchError(f"HTTP {error.code}: {detail}") from None
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            if attempt + 1 < retries:
                time.sleep(delay * 2 ** (attempt + 1))
                continue
            raise FetchError(str(error)) from None
    raise FetchError("unreachable")


def identify(ticker: str, get) -> dict:
    """The one security this ticker names, or a refusal saying why not."""
    rows = get("data/tickers", {"ticker": ticker}).get("data", [])
    priced = [r for r in rows if r.get("table") in ("stocks", "funds")]
    if not priced:
        raise FetchError("not a priced security in Sharadar's tickers table")
    if len({r.get("permaticker") for r in priced}) > 1:
        raise FetchError(
            f"names {len(priced)} different securities in Sharadar; a ticker is not an identity, "
            "and fetching by this one would mix them"
        )
    return priced[0]


def fetch_prices(ticker: str, table: str, start: str, get, page_size: int = PAGE) -> list[dict]:
    """Every row from `start` on, in date order, sliced by date rather than paged.

    A full page means there may be more: the next request starts the day after
    the last date received, rather than skipping an offset into the table.
    """
    rows: list[dict] = []
    since = start
    while True:
        page = get(
            f"data/{table}",
            {"ticker": ticker, "from": since, "fields": FIELDS, "sort": "date", "limit": page_size},
        ).get("data", [])
        rows.extend(page)
        if len(page) < page_size:
            return rows
        last = dt.date.fromisoformat(page[-1]["date"])
        since = (last + dt.timedelta(days=1)).isoformat()


def to_bars(rows: list[dict], closing_bell) -> list[tuple]:
    bars = []
    for row in rows:
        close = row.get("close")
        if close is None or not close > 0:
            continue  # a bar with no price is not a bar
        bars.append((
            closing_bell(dt.date.fromisoformat(row["date"])),
            float(row.get("open") or close),
            float(row.get("high") or close),
            float(row.get("low") or close),
            float(close),
            float(row.get("volume") or 0.0),
        ))
    return bars


def to_total_return_bars(rows: list[dict], closing_bell) -> list[tuple]:
    """Each bar scaled by closeadj / close, so the closes compound to total return."""
    bars = []
    for row in rows:
        close, adjusted = row.get("close"), row.get("closeadj")
        if close in (None, "") or adjusted in (None, ""):
            continue
        close, adjusted = float(close), float(adjusted)
        if not (close > 0 and adjusted > 0):
            continue
        factor = adjusted / close
        bars.append((
            closing_bell(dt.date.fromisoformat(row["date"])),
            float(row.get("open") or close) * factor,
            float(row.get("high") or close) * factor,
            float(row.get("low") or close) * factor,
            adjusted,
            float(row.get("volume") or 0.0),
        ))
    return bars


def rebuild_total_return(out: Path, writer) -> int:
    """SYMBOL.TR.msb for every ticker already in csv/, without fetching anything."""
    count = 0
    for path in sorted((out / "csv").glob("*.csv")):
        with path.open(newline="", encoding="utf-8") as handle:
            rows = sorted(csv.DictReader(handle), key=lambda row: row["date"])
        bars = to_total_return_bars(rows, writer.closing_bell)
        if bars:
            writer.write_msb(out / f"{path.stem}.TR.msb", f"{path.stem}.TR", bars)
            print(f"{path.stem:8} {len(bars):6} total-return bars")
            count += 1
    return count


def write_csv(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS.split(","), extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def load_manifest(out: Path) -> dict:
    path = out / "manifest.json"
    if path.exists():
        return json.loads(path.read_text(encoding="utf-8"))
    return {
        "source": "sharadar",
        "terms_url": "https://sharadar.com/terms",
        "licence": "personal use; not redistributable; publish aggregate results only",
        "adjustment": "SYMBOL.msb is split-adjusted (Sharadar close); SYMBOL.TR.msb is total return (closeadj); csv/ has every column",
        "tickers": {},
    }


def save_manifest(out: Path, manifest: dict) -> None:
    path = out / "manifest.json"
    temporary = path.with_suffix(".json.part")
    temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def fetch(tickers: list[str], out: Path, start: str, refresh: bool, delay: float, get, writer) -> int:
    out.mkdir(parents=True, exist_ok=True)
    manifest = load_manifest(out)
    failures = 0
    for ticker in tickers:
        ticker = ticker.strip().upper()
        if not ticker:
            continue
        entry = manifest["tickers"].get(ticker, {})
        if entry.get("status") == "ok" and (out / f"{ticker}.msb").exists() and not refresh:
            print(f"{ticker:8} cached ({entry.get('rows')} bars), skipped")
            continue
        now = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
        try:
            security = identify(ticker, get)
            rows = fetch_prices(ticker, security["table"], start, get)
            bars = to_bars(rows, writer.closing_bell)
            if not bars:
                raise FetchError("no priced bars returned")
            writer.write_msb(out / f"{ticker}.msb", ticker, bars)
            total_return = to_total_return_bars(rows, writer.closing_bell)
            if total_return:
                writer.write_msb(out / f"{ticker}.TR.msb", f"{ticker}.TR", total_return)
            write_csv(out / "csv" / f"{ticker}.csv", rows)
            manifest["tickers"][ticker] = {
                "status": "ok",
                "table": security["table"],
                "permaticker": security.get("permaticker"),
                "name": security.get("name"),
                "isdelisted": security.get("isdelisted"),
                "firstpricedate": security.get("firstpricedate"),
                "lastpricedate": security.get("lastpricedate"),
                "rows": len(bars),
                "first": rows[0]["date"],
                "last": rows[-1]["date"],
                "fetched_at": now,
            }
            print(f"{ticker:8} {len(bars):6} bars  {rows[0]['date']} to {rows[-1]['date']}  ({security['table']})")
        except FetchError as error:
            failures += 1
            manifest["tickers"][ticker] = {"status": "failed", "error": str(error), "fetched_at": now}
            print(f"{ticker:8} FAILED  {error}")
        # Saved after every ticker, so an interrupted run resumes rather than restarts.
        save_manifest(out, manifest)
        time.sleep(delay)
    return failures


def show_status(out: Path) -> int:
    manifest = load_manifest(out)
    if not manifest["tickers"]:
        print(f"nothing fetched into {out}")
        return 0
    for ticker, entry in sorted(manifest["tickers"].items()):
        if entry["status"] == "ok":
            print(f"{ticker:8} ok      {entry['rows']:6} bars  {entry['first']} to {entry['last']}")
        else:
            print(f"{ticker:8} failed  {entry['error']}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tickers", nargs="*", default=[])
    parser.add_argument("--tickers-file", type=Path)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help=f"cache directory, outside the repository (default {DEFAULT_OUT})")
    parser.add_argument("--from", dest="start", default="1990-01-01", help="first date to ask for")
    parser.add_argument("--refresh", action="store_true", help="fetch again even if cached")
    parser.add_argument("--delay", type=float, default=0.5, help="seconds between requests")
    parser.add_argument("--status", action="store_true", help="list what is cached and stop")
    parser.add_argument("--rebuild-tr", action="store_true",
                        help="write SYMBOL.TR.msb from the cached csv/ files, fetching nothing")
    args = parser.parse_args(argv)

    try:
        out = check_out_dir(args.out)
        if args.status:
            return show_status(out)
        if args.rebuild_tr:
            rebuild_total_return(out, load_writer())
            return 0
        tickers = list(args.tickers)
        if args.tickers_file:
            tickers += [line.split("#")[0].strip() for line in args.tickers_file.read_text().splitlines()]
        tickers = [t for t in tickers if t]
        if not tickers:
            parser.error("give --tickers or --tickers-file")
        key = api_key()
        writer = load_writer()

        def get(path: str, params: dict) -> dict:
            response = get_json(path, params, key, args.delay)
            time.sleep(args.delay)
            return response

        failures = fetch(tickers, out, args.start, args.refresh, args.delay, get, writer)
    except FetchError as error:
        print(f"fetch_sharadar: {error}", file=sys.stderr)
        return 2
    print(f"wrote {out}; {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
