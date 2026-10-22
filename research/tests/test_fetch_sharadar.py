"""scripts/fetch_sharadar.py, offline: a fake API stands in for Sharadar.

What is tested is what the policy asks of a fetcher of licensed data
(docs/DATA_POLICY.md): the key from the environment only, the cache outside the
repository, a reused ticker refused rather than guessed at, a failure recorded
rather than skipped, and requests sliced by date rather than paged.
"""

from __future__ import annotations

import datetime as dt
import importlib.util
import json
import struct
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def _load():
    spec = importlib.util.spec_from_file_location("fetch_sharadar", REPO / "scripts" / "fetch_sharadar.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


fs = _load()
WRITER = fs.load_writer()


def rows(start: str, n: int) -> list[dict]:
    day = dt.date.fromisoformat(start)
    out = []
    while len(out) < n:
        if day.weekday() < 5:
            close = 100.0 + len(out)
            out.append({"date": day.isoformat(), "open": close, "high": close + 1, "low": close - 1,
                        "close": close, "volume": 1000.0, "closeadj": close * 0.98, "closeunadj": close})
        day += dt.timedelta(days=1)
    return out


class FakeApi:
    """Answers like Sharadar's /data routes, and records every request."""

    def __init__(self, tickers: dict[str, list[dict]], prices: dict[str, list[dict]], page: int = 10_000):
        self.tickers = tickers
        self.prices = prices
        self.page = page
        self.calls: list[tuple[str, dict]] = []

    def __call__(self, path: str, params: dict) -> dict:
        self.calls.append((path, dict(params)))
        if path == "data/tickers":
            return {"data": self.tickers.get(params["ticker"], [])}
        if params["ticker"] not in self.prices:
            raise fs.FetchError("HTTP 403: Exceeds free tier")
        rows_ = [r for r in self.prices[params["ticker"]] if r["date"] >= params["from"]]
        return {"data": rows_[: min(self.page, params["limit"])]}


def security(ticker: str, table: str = "stocks", permaticker: str = "1") -> dict:
    return {"ticker": ticker, "table": table, "permaticker": permaticker, "name": ticker,
            "isdelisted": "N", "firstpricedate": "2020-01-02", "lastpricedate": "2020-12-31"}


def test_the_key_comes_from_the_environment_and_nowhere_else(monkeypatch):
    monkeypatch.delenv(fs.KEY_VARIABLE, raising=False)
    with pytest.raises(fs.FetchError, match=fs.KEY_VARIABLE):
        fs.api_key()
    monkeypatch.setenv(fs.KEY_VARIABLE, "abc")
    assert fs.api_key() == "abc"


def test_a_cache_inside_the_repository_is_refused(tmp_path):
    with pytest.raises(fs.FetchError, match="inside the repository"):
        fs.check_out_dir(REPO / "data" / "cache" / "sharadar")
    with pytest.raises(fs.FetchError, match="inside the repository"):
        fs.check_out_dir(REPO)
    assert fs.check_out_dir(tmp_path) == tmp_path.resolve()


def test_the_default_cache_is_outside_the_repository():
    assert REPO not in fs.DEFAULT_OUT.resolve().parents


def test_a_ticker_naming_two_securities_is_refused():
    api = FakeApi({"XYZ": [security("XYZ", permaticker="1"), security("XYZ", permaticker="2")]}, {})
    with pytest.raises(fs.FetchError, match="ticker is not an identity"):
        fs.identify("XYZ", api)


def test_long_histories_are_sliced_by_date_not_paged():
    api = FakeApi({"AAA": [security("AAA")]}, {"AAA": rows("2000-01-03", 25)}, page=10)
    out = fs.fetch_prices("AAA", "stocks", "1990-01-01", api, page_size=10)
    assert len(out) == 25
    assert [r["date"] for r in out] == sorted({r["date"] for r in out})
    price_calls = [p for path, p in api.calls if path == "data/stocks"]
    assert len(price_calls) == 3
    assert all("offset" not in p and "skip" not in p for p in price_calls)
    assert price_calls[1]["from"] > price_calls[0]["from"]


def test_a_fetch_writes_the_cache_and_records_every_outcome(tmp_path):
    api = FakeApi(
        {"AAA": [security("AAA")], "FND": [security("FND", table="funds")], "NOPE": [security("NOPE")]},
        {"AAA": rows("2020-01-02", 30), "FND": rows("2020-01-02", 20)},
    )
    failures = fs.fetch(["AAA", "FND", "NOPE", "GONE"], tmp_path, "1990-01-01", False, 0.0, api, WRITER)
    assert failures == 2

    manifest = json.loads((tmp_path / "manifest.json").read_text())
    entries = manifest["tickers"]
    assert entries["AAA"]["status"] == "ok" and entries["AAA"]["rows"] == 30
    assert entries["FND"]["table"] == "funds"
    assert "Exceeds free tier" in entries["NOPE"]["error"]
    assert "not a priced security" in entries["GONE"]["error"]
    assert manifest["terms_url"] == "https://sharadar.com/terms"

    # The .msb carries the split-adjusted close, stamped at the closing bell.
    data = (tmp_path / "AAA.msb").read_bytes()
    assert data[:4] == b"MSB1"
    count = struct.unpack_from("<Q", data, 8)[0]
    assert count == 30
    ts, _, _, _, close, _ = struct.unpack_from("<qddddd", data, 64)
    assert ts == WRITER.closing_bell(dt.date(2020, 1, 2))
    assert close == 100.0

    # Every delivered column survives in the CSV, closeadj included.
    header = (tmp_path / "csv" / "AAA.csv").read_text().splitlines()[0]
    assert header == fs.FIELDS


def test_a_cached_ticker_is_not_fetched_again_unless_asked(tmp_path):
    api = FakeApi({"AAA": [security("AAA")]}, {"AAA": rows("2020-01-02", 5)})
    fs.fetch(["AAA"], tmp_path, "1990-01-01", False, 0.0, api, WRITER)
    before = len(api.calls)
    fs.fetch(["AAA"], tmp_path, "1990-01-01", False, 0.0, api, WRITER)
    assert len(api.calls) == before
    fs.fetch(["AAA"], tmp_path, "1990-01-01", True, 0.0, api, WRITER)
    assert len(api.calls) > before
