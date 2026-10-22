"""What price data the service holds, for the assistant's data tools.

This is not the data catalog of CATALOG.md (part R1): there are no ticker
histories or renames yet. It answers from what is on disk: the daily
directories' SYMBOL.msb files, with names and licence from each fetcher's
manifest.json when it is there (a rate series from FRED is a security with
asset_class "rate"), the CSV sources, and the synthetic recipes
scripts/build_recipes.py wrote, each with the validation it measured. Every date
it reports is read from the files, so the assistant never has to state one from
memory -- the rule in its system prompt.

SYMBOL.TR.msb, a total-return series, is another series of SYMBOL's, not a
security of its own, and is not listed.

A security's id is its ticker in lower case, which is what the job binds a
form's `security_id` to today.
"""

from __future__ import annotations

import csv
import datetime as dt
import json
import struct
from dataclasses import asdict, dataclass
from pathlib import Path

from .config import Settings

# scripts/fetch_stooq.py's layout, which the engine reads.
_HEADER = struct.Struct("<II Q 32s q q")
_RECORD = struct.Struct("<q ddddd")
_MAGIC = struct.unpack("<I", b"MSB1")[0]


@dataclass(frozen=True)
class Security:
    security_id: str
    symbol: str
    name: str
    asset_class: str
    first_bar: str
    last_bar: str
    bars: int
    source: str
    licence: str
    status: str = "active"


def _date(nanos: int) -> str:
    # Bars are stamped at the 16:00 New York close, which is the same calendar
    # date in UTC. Counted from the epoch rather than through fromtimestamp,
    # which fails on Windows before 1970 -- and a rate series starts in 1954.
    return (dt.date(1970, 1, 1) + dt.timedelta(days=nanos // 86_400_000_000_000)).isoformat()


def _read_msb(path: Path) -> tuple[int, str, str] | None:
    with path.open("rb") as handle:
        header = handle.read(_HEADER.size)
        if len(header) < _HEADER.size:
            return None
        magic, _version, count, _ticker, _interval, _ = _HEADER.unpack(header)
        if magic != _MAGIC or count == 0:
            return None
        first = _RECORD.unpack(handle.read(_RECORD.size))[0]
        handle.seek(_HEADER.size + (count - 1) * _RECORD.size)
        last = _RECORD.unpack(handle.read(_RECORD.size))[0]
    return count, _date(first), _date(last)


def _read_csv(path: Path) -> tuple[int, str, str] | None:
    with path.open(newline="", encoding="utf-8") as handle:
        dates = [row[0] for row in csv.reader(handle) if row and row[0][:1].isdigit()]
    return (len(dates), dates[0], dates[-1]) if dates else None


class Catalog:
    def __init__(self, settings: Settings):
        self._securities: dict[str, Security] = {}
        for directory in settings.daily_dirs():
            if directory.is_dir():
                self._load_daily(directory)
        self._recipes: list[dict] = []
        if settings.recipes is not None and settings.recipes.exists():
            self._recipes = [r for r in json.loads(settings.recipes.read_text(encoding="utf-8"))
                             if r.get("record") == "synthetic_recipe" and r.get("validation")]
        for symbol, path in settings.csv_sources:
            span = _read_csv(path) if path.exists() else None
            if span:
                self._add(symbol, "", "unknown", span, "csv", "")

    def _load_daily(self, directory: Path) -> None:
        manifest = {}
        if (directory / "manifest.json").exists():
            manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
        tickers = manifest.get("tickers", {})
        rates = manifest.get("series", {})
        for path in sorted(directory.glob("*.msb")):
            if path.stem.endswith(".TR") or path.stem.lower() in self._securities:
                continue  # another series of a security, or one an earlier directory answers for
            span = _read_msb(path)
            if span is None:
                continue
            if path.stem in rates:
                self._add(path.stem, rates[path.stem].get("title", ""), "rate", span,
                          manifest.get("source", "daily"), manifest.get("terms_url", ""))
                continue
            entry = tickers.get(path.stem, {})
            kind = {"funds": "etf", "stocks": "stock"}.get(entry.get("table", ""), "unknown")
            self._add(path.stem, entry.get("name", ""), kind, span,
                      manifest.get("source", "daily"), manifest.get("licence", ""),
                      "delisted" if entry.get("isdelisted") == "Y" else "active")

    def _add(self, symbol, name, asset_class, span, source, licence, status="active") -> None:
        bars, first, last = span
        self._securities[symbol.lower()] = Security(
            security_id=symbol.lower(), symbol=symbol, name=name, asset_class=asset_class,
            first_bar=first, last_bar=last, bars=bars, source=source, licence=licence,
            status=status)

    # --- The tools ----------------------------------------------------------

    def search(self, query: str, limit: int = 10) -> list[dict]:
        q = query.strip().lower()
        hits = [s for s in self._securities.values()
                if not q or q in s.symbol.lower() or q in s.name.lower()]
        hits.sort(key=lambda s: (s.symbol.lower() != q, s.symbol))
        return [self._record(s) for s in hits[:limit]]

    def resolve(self, ticker: str, as_of: str) -> dict:
        security = self._securities.get(ticker.strip().lower())
        if security is None:
            return {"security": None, "note": f"No data is held for {ticker}."}
        if as_of < security.first_bar:
            note = f"The data for {security.symbol} starts on {security.first_bar}."
            recipes = [r for r in self._recipes if r["target_security_id"] == security.security_id]
            for r in recipes:
                note += (f" Synthetic recipe {r['recipe_id']} extends it back to {r['available_from']};"
                         " see get_coverage.")
            return {"security": None, "note": note}
        return {"security": self._record(security)}

    def coverage(self, security_id: str) -> dict | None:
        security = self._securities.get(security_id.strip().lower())
        if security is None:
            return None
        is_rate = security.asset_class == "rate"
        return {
            **asdict(security),
            # Not measured here; the catalog will record them.
            "gaps": "not checked",
            "adjustments": "a rate in percent a year, as published" if is_rate
            else "split-adjusted closes; dividends are not included",
            "price_adjustment": None if is_rate else "split_only",
            "quality_tier": "exercise_only",
            "synthetic_recipes": [r for r in self._recipes if r["target_security_id"] == security.security_id],
        }

    @staticmethod
    def _record(s: Security) -> dict:
        # ticker_history is the catalog's shape; with no catalog it is the one
        # ticker the file is named by, over the file's own dates.
        return {
            "security_id": s.security_id, "name": s.name, "asset_class": s.asset_class,
            "status": s.status,
            "ticker_history": [{"ticker": s.symbol, "from": s.first_bar, "to": None}],
        }
