"""The research job's grading, held against the engine's.

data/samples/research/job_trace.json is the engine's trace of a job over the
drawdown_entry fixture, and job_results.json the Results it produced; both are
written by `ms_research --trace --fixed-time` and committed. research/grading.py
regrades the trace from JOB.md alone, and this test requires the two to agree:
exactly, except for quantities computed with pow, which JOB.md allows a relative
1e-12 because pow is not correctly rounded on every platform. Every decision --
the selection, every gate, the grade -- must agree exactly regardless.

The engine's side of the same fixture is tests/test_research_job.cpp, which
requires a fresh run to reproduce both files byte for byte.
"""

from __future__ import annotations

import json
import math
import pathlib

import pytest

from research import grading

FIXTURES = pathlib.Path(__file__).resolve().parents[2] / "data" / "samples" / "research"
TRACE = json.loads((FIXTURES / "job_trace.json").read_text(encoding="utf-8"))
RESULTS = json.loads((FIXTURES / "job_results.json").read_text(encoding="utf-8"))
GRADED = grading.grade(TRACE)

# pow reaches these. JOB.md, "Arithmetic".
POW_METRICS = {"cagr", "calmar"}
TAINTED_KEYS = {"cagr_pct", "calmar", "p_value_corrected", "cagr_p5_pct", "cagr_p50_pct",
                "cagr_p95_pct"}


def same(a, b, tainted: bool) -> bool:
    if a is None or b is None:
        return a is None and b is None
    a, b = float(a), float(b)
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    if a == b:
        return True
    return tainted and abs(a - b) <= 1e-12 * max(abs(a), abs(b))


def nullable(x):
    return None if isinstance(x, float) and math.isnan(x) else x


def assert_metrics(engine: dict, research: dict, where: str):
    for k, v in engine.items():
        assert same(v, nullable(research[k]), k in TAINTED_KEYS), f"{where}/{k}: {v} vs {research[k]}"


def test_the_form_hash_is_the_same_canonical_form():
    assert GRADED["form_sha256"] == RESULTS["run"]["form_sha256"]


def test_the_generator_reproduces_the_first_bootstrap_path_exactly():
    assert GRADED["first_path_closes"] == TRACE["bootstrap"]["first_path_closes"]


def test_the_grade_and_every_gate_agree():
    verdict = RESULTS["verdict"]
    assert GRADED["grade"] == verdict["grade"]
    assert GRADED["deciding_gate"] == verdict["deciding_gate"]
    assert GRADED["gates"] == [(g["id"], g["status"]) for g in verdict["gates"]]


def test_the_same_point_is_selected():
    surface = RESULTS["stats"]["parameter_surface"]
    assert GRADED["selected_index"] == surface["selected_index"]
    tainted = TRACE["metric"] in POW_METRICS
    for i, point in enumerate(surface["points"]):
        assert same(point["in_sample_metric"], nullable(GRADED["objective"][i]), tainted), i
        assert point["excluded"] == (any(GRADED["eligible"]) and not GRADED["eligible"][i])


def test_the_selected_point_s_metrics_agree_in_every_window():
    for name, window in RESULTS["stats"]["selected"]["windows"].items():
        mine = GRADED["selected_windows"][name]
        assert (window["from"], window["to"]) == (mine["from"], mine["to"])
        assert_metrics(window["metrics"], mine["metrics"], f"selected/{name}")


def test_the_benchmarks_metrics_agree_in_every_window():
    for engine, mine in zip(RESULTS["stats"]["benchmarks"], GRADED["benchmark_windows"]):
        for name, window in engine["windows"].items():
            assert_metrics(window["metrics"], mine[name]["metrics"], f"{engine['symbol']}/{name}")


def test_the_random_timing_null_agrees():
    tainted = TRACE["metric"] in POW_METRICS
    for k, v in RESULTS["stats"]["random_timing"].items():
        if k == "metric":
            continue
        assert same(v, nullable(GRADED["random_timing"][k]), tainted or k in TAINTED_KEYS), k


def test_the_correction_and_the_plateau_agree():
    surface = RESULTS["stats"]["parameter_surface"]
    assert surface["effective_n"] == GRADED["effective_n"]
    assert same(surface["plateau_ratio"], nullable(GRADED["plateau_ratio"]),
                TRACE["metric"] in POW_METRICS)


def test_the_outcome_range_agrees():
    boot = RESULTS["stats"]["bootstrap"]
    for side in ("strategy", "benchmark"):
        engine = [boot[side][k] for k in ("cagr_p5_pct", "cagr_p50_pct", "cagr_p95_pct")]
        for a, b in zip(engine, GRADED["bootstrap"][side]):
            assert same(a, nullable(b), True)
    assert boot["prob_beats_benchmark"] == GRADED["bootstrap"]["prob_beats_benchmark"]
    assert (boot["share_paths_below_minus_90_pct"]
            == GRADED["bootstrap"]["share_paths_below_minus_90_pct"])


def test_the_trades_agree():
    engine = RESULTS["stats"]["trades"]
    assert len(engine) == len(GRADED["trades"])
    for a, b in zip(engine, GRADED["trades"]):
        assert (a["entry_date"], a["exit_date"]) == (b["entry_date"], b["exit_date"])
        assert a["return_pct"] == b["return_pct"]


def test_the_fixture_exercises_the_interesting_paths():
    # A fixture where nothing is swept, no gate fails, or no trade happens would
    # let most of the grading go uncompared.
    assert TRACE["dims"]
    assert len(RESULTS["stats"]["trades"]) >= 5
    statuses = {status for _, status in GRADED["gates"]}
    assert {"pass", "fail"} <= statuses
    assert RESULTS["stats"]["random_timing"]["n_simulations"] == TRACE["null_draws"]


def test_the_minimum_trade_rule_decides_the_fixture_s_selection():
    # The rule is only tested if it changes the answer: the best in-sample point
    # overall must be one the rule excludes.
    objective, eligible = GRADED["objective"], GRADED["eligible"]
    overall = max(range(len(objective)), key=lambda i: (objective[i], -i))
    assert not eligible[overall]
    assert GRADED["selected_index"] != overall


def test_ties_with_the_null_count_half():
    # On a flat price every alternative has the same frictionless curve as the
    # strategy, so every draw ties: percentile 50, p-value 1.
    days = list(range(10_000, 10_040))
    series = grading.Series(days, [100.0] * len(days), 0, len(days), len(days) - 1)
    run = grading.analyse(
        [10_000.0] * len(days),
        [{"bar": 5, "side": "buy", "quantity": 1.0, "price": 100.0, "tag": 0x101,
          "commission": 0.0},
         {"bar": 15, "side": "sell", "quantity": 1.0, "price": 100.0, "tag": 0x201,
          "commission": 0.0}],
        series.closes, series.e,
    )
    null = grading.random_timing(series, run, "total_return", 50, grading.SplitMix64(7))
    assert null["percentile"] == 50.0
    assert null["p"] == 1.0


@pytest.mark.parametrize(
    "seed, first, second",
    [(0, 0xE220A8397B1DCDAF, 0x6E789E6AA1B965F4)],
)
def test_splitmix64_against_its_reference_outputs(seed, first, second):
    rng = grading.SplitMix64(seed)
    assert rng.next() == first
    assert rng.next() == second


def test_percentile_interpolates_and_drops_nulls():
    assert grading.percentile([3.0, 1.0, float("nan"), 2.0], 0.5) == 2.0
    assert grading.percentile([1.0, 2.0], 0.25) == 1.25
    assert grading.percentile([5.0], 0.95) == 5.0
    assert math.isnan(grading.percentile([], 0.5))
