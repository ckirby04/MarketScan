#!/usr/bin/env python3
"""Fetches daily rate series from FRED into a cache outside the repository.

    $env:FRED_API_KEY = "..."
    python scripts/fetch_fred.py                 # DTB3, the 3-month T-bill
    python scripts/fetch_fred.py --series DTB3 DGS10

FRED (https://fred.stlouisfed.org), the Federal Reserve Bank of St. Louis's data
service, is an approved source under docs/DATA_POLICY.md section 5: an API key
and documented terms (https://fred.stlouisfed.org/docs/api/terms_of_use.html).
A rate series is what the synthetic recipes finance leverage at (CATALOG.md,
"Synthetic recipe") and, later, what cash earns.

  * The key comes from FRED_API_KEY and nowhere else. FRED takes it only as a
    query parameter, so the request URL holds it: the URL is never printed,
    logged or put in an error message.
  * The cache lives outside the repository -- by default ../marketscan-data/fred
    -- and a path inside it is refused, as for every fetched source.
  * One request per series, with a delay and backoff.

What is written, per series, under --out:

  SERIES.csv     date,value as FRED delivers it, missing days (".") left out
  SERIES.msb     the same values as daily bars (open = high = low = close =
                 the rate, in percent a year), for the engine
  manifest.json  what each series is (title, units, frequency, source notes)
                 and when it was fetched
"""

from __future__ import annotations

import argparse
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
API = "https://api.stlouisfed.org/fred"
KEY_VARIABLE = "FRED_API_KEY"
DEFAULT_OUT = REPO.parent / "marketscan-data" / "fred"
USER_AGENT = "marketscan-fetch-fred/0.1"
NOTICE = "This product uses the FRED API but is not endorsed or certified by the Federal Reserve Bank of St. Louis."


class FetchError(RuntimeError):
    pass


def load_writer():
    """fetch_stooq's .msb writer and closing-bell stamp, so there is one copy."""
    spec = importlib.util.spec_from_file_location("marketscan_fetch", REPO / "scripts" / "fetch_stooq.py")
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
        raise FetchError(f"{out} is inside the repository; fetched data lives outside it "
                         "(docs/DATA_POLICY.md section 4). Pass --out somewhere else.")
    return resolved


def tls_context() -> ssl.SSLContext:
    """Verification always on, against certifi's bundle when installed (see fetch_sharadar.py)."""
    try:
        import certifi
    except ImportError:
        return ssl.create_default_context()
    return ssl.create_default_context(cafile=certifi.where())


def get_json(path: str, params: dict, key: str, delay: float, retries: int = 4) -> dict:
    url = f"{API}/{path}?{urllib.parse.urlencode({**params, 'api_key': key, 'file_type': 'json'})}"
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    for attempt in range(retries + 1):
        time.sleep(delay)
        try:
            with urllib.request.urlopen(request, timeout=60, context=tls_context()) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            # The message names the endpoint and status, never the URL: it holds the key.
            if error.code in (429, 500, 502, 503, 504) and attempt < retries:
                delay = max(delay * 2, 2.0)
                continue
            try:
                detail = json.load(error).get("error_message", "")
            except Exception:
                detail = ""
            raise FetchError(f"FRED {path} answered {error.code}{': ' + detail if detail else ''}") from None
        except urllib.error.URLError as error:
            if attempt < retries:
                delay = max(delay * 2, 2.0)
                continue
            raise FetchError(f"FRED {path}: {error.reason}") from None
    raise FetchError(f"FRED {path}: gave up after {retries} retries")


def observations(raw: list[dict]) -> list[tuple[str, float]]:
    """FRED's observations as (date, value), without its "." for a missing day."""
    out = []
    for row in raw:
        if row["value"] in (".", ""):
            continue
        out.append((row["date"], float(row["value"])))
    out.sort()
    return out


def fetch(series_ids: list[str], out: Path, delay: float, get, writer) -> int:
    out.mkdir(parents=True, exist_ok=True)
    manifest_path = out / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {
        "source": "fred", "terms_url": "https://fred.stlouisfed.org/docs/api/terms_of_use.html",
        "notice": NOTICE, "series": {}}
    failures = 0
    for series_id in series_ids:
        try:
            info = get("series", {"series_id": series_id})["seriess"][0]
            rows = observations(get("series/observations", {"series_id": series_id})["observations"])
        except FetchError as error:
            manifest["series"][series_id] = {"status": "failed", "note": str(error)}
            print(f"{series_id}: {error}", file=sys.stderr)
            failures += 1
            continue
        if not rows:
            manifest["series"][series_id] = {"status": "failed", "note": "no observations"}
            failures += 1
            continue
        with (out / f"{series_id}.csv").open("w", encoding="utf-8", newline="\n") as handle:
            handle.write("date,value\n")
            for date, value in rows:
                handle.write(f"{date},{value!r}\n")
        bars = [(writer.closing_bell(dt.date.fromisoformat(d)), v, v, v, v, 0.0) for d, v in rows]
        writer.write_msb(out / f"{series_id}.msb", series_id, bars)
        manifest["series"][series_id] = {
            "status": "ok", "title": info.get("title", ""), "units": info.get("units", ""),
            "frequency": info.get("frequency", ""), "notes": info.get("notes", ""),
            "first": rows[0][0], "last": rows[-1][0], "rows": len(rows),
            "fetched_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        }
        print(f"{series_id}: {len(rows)} observations, {rows[0][0]} to {rows[-1][0]}")
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--series", nargs="+", default=["DTB3"])
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--delay", type=float, default=0.5)
    args = parser.parse_args(argv)
    try:
        out = check_out_dir(args.out)
        key = api_key()
    except FetchError as error:
        print(error, file=sys.stderr)
        return 2

    def get(path: str, params: dict) -> dict:
        return get_json(path, params, key, args.delay)

    return fetch(args.series, out, args.delay, get, load_writer())


if __name__ == "__main__":
    sys.exit(main())
