"""research/synthetic.py, scripts/build_recipes.py and scripts/fetch_fred.py.

The cross-test fixture first: synthetic.py must reproduce
data/samples/research/synthetic_cases.json exactly, the file the engine's
extend_history is held to by tests/test_research_synthetic.cpp. Then what the
validation means, measured on cases whose answer is known, and what the two
scripts promise about keys, paths and recipes without a validation.
"""

from __future__ import annotations

import datetime as dt
import importlib.util
import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from research import synthetic  # noqa: E402
from research.contracts import CONTRACTS, validator  # noqa: E402

CASES = json.loads((REPO / "data" / "samples" / "research" / "synthetic_cases.json").read_text())["cases"]


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


build_recipes = _load("build_recipes")
fetch_fred = _load("fetch_fred")
fetch_sharadar = _load("fetch_sharadar")
WRITER = fetch_sharadar.load_writer()


def weekdays(n: int, start=dt.date(2001, 1, 1)) -> list[str]:
    out, day = [], start
    while len(out) < n:
        if day.weekday() < 5:
            out.append(day.isoformat())
        day += dt.timedelta(days=1)
    return out


# --- The cross-test fixture ------------------------------------------------------

@pytest.mark.parametrize("case", CASES, ids=[c["name"] for c in CASES])
def test_synthetic_py_reproduces_the_cross_test_fixture_exactly(case):
    i = case["inputs"]
    p = synthetic.Params(**{k: i["params"][k] for k in ("leverage", "annual_fee_pct", "borrow_spread_pct")})
    expected = case["expected"]
    if "error" in expected:
        with pytest.raises(synthetic.RecipeError) as raised:
            synthetic.extend(i["underlying_days"], i["underlying"], i["rate_days"], i["rates"],
                             i["first_real_day"], i["first_real_close"], p)
        assert str(raised.value) == expected["error"]
        return
    days, closes = synthetic.extend(i["underlying_days"], i["underlying"], i["rate_days"], i["rates"],
                                    i["first_real_day"], i["first_real_close"], p)
    assert days == expected["days"]
    assert closes == expected["closes"]  # exact: the engine is held to the same doubles


def test_the_fixture_has_both_kinds_of_case():
    assert sum("error" in c["expected"] for c in CASES) >= 2
    assert sum("days" in c["expected"] for c in CASES) >= 4


# --- What the recipe and its validation mean ----------------------------------------

def test_the_history_ends_at_the_first_real_close_and_steps_back_by_the_formula():
    days = weekdays(4)
    u = [100.0, 101.0, 99.0, 102.0]
    p = synthetic.Params(3, 0.95, 0.5)
    out_days, out = synthetic.extend(days, u, days, [2.0] * 4, days[3], 50.0, p)
    assert out_days == days[:3]
    r3 = synthetic.daily_return(u[2], u[3], 2.0, p)
    assert out[2] == 50.0 / (1.0 + r3)
    assert synthetic.daily_return(100.0, 100.0, 2.0, p) == -(0.95 + 2 * 2.5) / 100 / 252


def test_the_rate_used_is_the_last_one_on_or_before_the_interval_start():
    rate_days = ["2001-01-01", "2001-01-03"]
    assert synthetic.rate_on_or_before(rate_days, [1.0, 3.0], "2001-01-02") == 1.0
    assert synthetic.rate_on_or_before(rate_days, [1.0, 3.0], "2001-01-03") == 3.0
    assert synthetic.rate_on_or_before(rate_days, [1.0, 3.0], "2000-12-29") is None


def test_a_perfect_recipe_validates_as_perfect():
    days = weekdays(300)
    u, price = [], 100.0
    for k in range(300):
        price *= 1.0 + (0.01 if k % 3 else -0.012)
        u.append(price)
    p = synthetic.Params(3, 0.9, 0.5)
    real = [50.0]
    for k in range(1, 300):
        real.append(real[-1] * (1.0 + synthetic.daily_return(u[k - 1], u[k], 2.0, p)))
    v = synthetic.validate(days, u, days, [2.0] * 300, days, real, p)
    assert v["daily_return_correlation"] == pytest.approx(1.0, abs=1e-12)
    assert v["annualized_tracking_difference_pct"] == pytest.approx(0.0, abs=1e-9)
    assert v["max_cumulative_gap_pct"] == pytest.approx(0.0, abs=1e-9)
    assert (v["overlap_from"], v["overlap_to"]) == (days[0], days[-1])


def test_a_fee_left_out_shows_as_tracking_difference_of_about_that_fee():
    days = weekdays(504)
    u = [100.0 * (1.0 + 0.0005) ** k for k in range(504)]
    true = synthetic.Params(2, 1.0, 0.0)
    real = [50.0]
    for k in range(1, 504):
        real.append(real[-1] * (1.0 + synthetic.daily_return(u[k - 1], u[k], 0.0, true)))
    v = synthetic.validate(days, u, days, [0.0] * 504, days, real, synthetic.Params(2, 0.0, 0.0))
    # The synthetic, charged nothing, beats the real fund by about the 1% fee a year.
    assert 0.9 < v["annualized_tracking_difference_pct"] < 1.3


def test_only_intervals_both_series_step_over_are_compared():
    days = weekdays(6)
    u = [100.0, 101, 102, 103, 104, 105]
    target_days = [days[0], days[2], days[3], days[4], days[5]]  # the target skips a day
    target = [10.0, 10.5, 10.6, 10.7, 10.8]
    v = synthetic.validate(days, u, days, [1.0] * 6, target_days, target, synthetic.Params(2, 0, 0))
    assert v["overlap_from"] == days[2]  # the skipped interval is not compared


# --- build_recipes.py ---------------------------------------------------------------

def write(directory: Path, name: str, days: list[str], closes: list[float]) -> None:
    bars = [(WRITER.closing_bell(dt.date.fromisoformat(d)), c, c, c, c, 0.0) for d, c in zip(days, closes)]
    WRITER.write_msb(directory / f"{name}.msb", name, bars)


def test_a_recipe_is_built_with_its_validation_and_satisfies_the_catalog_schema(tmp_path):
    days = weekdays(400, dt.date(1999, 1, 1))
    u = [100.0 * (1.0 + (0.004 if k % 2 else -0.003)) ** 1 * (1.0002 ** k) for k in range(400)]
    write(tmp_path, "QQQ.TR", days, u)
    write(tmp_path, "DTB3", days, [3.0] * 400)
    write(tmp_path, "TQQQ", days[250:], [10.0 * 1.001 ** k for k in range(150)])
    read_msb = build_recipes.load_reader()
    record = build_recipes.build(build_recipes.RECIPES[0], [tmp_path], read_msb, "test")
    schema = json.loads((CONTRACTS / "catalog_records.schema.json").read_text(encoding="utf-8"))
    definition = {**schema, "$ref": "#/$defs/synthetic_recipe"}
    errors = [e.message for e in validator(definition).iter_errors(record)]
    assert errors == []
    assert record["available_from"] == days[0]
    assert record["validation"]["overlap_from"] == days[250]
    assert record["validation"]["computed_by"] == "test"
    assert record["params"]["fee_source"].startswith("ProShares")


def test_a_recipe_with_missing_inputs_is_not_built(tmp_path):
    with pytest.raises(synthetic.RecipeError, match="no data for"):
        build_recipes.build(build_recipes.RECIPES[0], [tmp_path], build_recipes.load_reader(), "test")


def test_recipes_are_not_written_inside_the_repository():
    assert build_recipes.main(["--out", str(REPO / "data" / "recipes.json")]) == 2


# --- fetch_fred.py --------------------------------------------------------------------

def test_the_fred_key_comes_from_the_environment_and_nowhere_else(monkeypatch):
    monkeypatch.delenv("FRED_API_KEY", raising=False)
    with pytest.raises(fetch_fred.FetchError, match="FRED_API_KEY"):
        fetch_fred.api_key()


def test_a_fred_cache_inside_the_repository_is_refused():
    with pytest.raises(fetch_fred.FetchError, match="inside the repository"):
        fetch_fred.check_out_dir(REPO / "data" / "fred")


def test_freds_missing_days_are_left_out():
    rows = [{"date": "2020-01-02", "value": "1.5"}, {"date": "2020-01-03", "value": "."},
            {"date": "2020-01-01", "value": "1.4"}]
    assert fetch_fred.observations(rows) == [("2020-01-01", 1.4), ("2020-01-02", 1.5)]


def test_a_fred_fetch_writes_csv_msb_and_manifest(tmp_path):
    def api(path, params):
        if path == "series":
            return {"seriess": [{"title": "3-Month Treasury Bill", "units": "Percent", "frequency": "Daily"}]}
        return {"observations": [{"date": "1954-01-04", "value": "1.33"}, {"date": "1954-01-05", "value": "."},
                                 {"date": "2020-03-02", "value": "0.0"}]}
    assert fetch_fred.fetch(["DTB3"], tmp_path, 0.0, api, WRITER) == 0
    manifest = json.loads((tmp_path / "manifest.json").read_text())
    assert manifest["series"]["DTB3"]["rows"] == 2
    assert manifest["notice"].startswith("This product uses the FRED API")
    _ticker, bars = WRITER.read_msb(tmp_path / "DTB3.msb")
    # A zero rate is a rate, and a 1954 date reads back as itself.
    assert bars == [("1954-01-04", 1.33), ("2020-03-02", 0.0)]


def test_a_fred_error_never_carries_the_url_and_its_key(monkeypatch):
    import urllib.error
    import urllib.request

    def refuse(request, timeout, context):
        raise urllib.error.HTTPError(request.full_url, 400, "Bad Request", {}, None)

    monkeypatch.setattr(urllib.request, "urlopen", refuse)
    with pytest.raises(fetch_fred.FetchError) as raised:
        fetch_fred.get_json("series", {"series_id": "DTB3"}, "SECRETKEY123", 0.0, retries=0)
    assert "SECRETKEY123" not in str(raised.value)
    assert "api_key" not in str(raised.value)


# --- fetch_sharadar.py's total-return bars ---------------------------------------------

def test_total_return_bars_are_scaled_to_closeadj():
    rows = [{"date": "2020-01-02", "open": 10, "high": 11, "low": 9, "close": 10, "volume": 5, "closeadj": 8},
            {"date": "2020-01-03", "open": 10, "high": 10, "low": 10, "close": 10, "volume": 5, "closeadj": None}]
    bars = fetch_sharadar.to_total_return_bars(rows, WRITER.closing_bell)
    assert len(bars) == 1  # no closeadj, no total-return bar
    _ts, o, h, low, c, _v = bars[0]
    assert (o, h, low, c) == (8.0, 8.8, 7.2, 8.0)
