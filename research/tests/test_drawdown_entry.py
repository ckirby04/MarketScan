"""The drawdown_entry rule, from the research layer's side.

The engine's half is tests/test_research_rule.cpp. Both are held to
data/samples/research/drawdown_expected.json; this side also regenerates that
file and requires it to match what is committed.
"""

from __future__ import annotations

import datetime as dt
import importlib.util
import json
import pathlib

import pytest

from research import drawdown_entry as rule

REPO = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = REPO / "data" / "samples" / "research"


def _generator():
    spec = importlib.util.spec_from_file_location(
        "make_drawdown_fixture", REPO / "scripts" / "make_drawdown_fixture.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GENERATOR = _generator()
DATES, CLOSES = GENERATOR.load_prices()
CASES = json.loads((FIXTURES / "drawdown_cases.json").read_text(encoding="utf-8"))["cases"]
EXPECTED = json.loads((FIXTURES / "drawdown_expected.json").read_text(encoding="utf-8"))


def test_the_committed_expectations_are_what_this_implementation_produces():
    assert GENERATOR.expected(DATES, CLOSES) == EXPECTED


def test_the_committed_prices_are_what_the_generator_produces():
    dates, closes = GENERATOR.make_path()
    assert dates == DATES
    assert closes == CLOSES


def test_every_branch_of_the_rule_is_reached_somewhere():
    # A cross-test over cases that never sell on a stop proves nothing about the
    # stop. Every exit reason, a multi-tranche order and a refusal must occur.
    reasons, masks, rejected = set(), set(), 0
    for result in EXPECTED.values():
        rejected += result["rejected"]
        for fill in result["fills"]:
            payload = fill["tag"] & 0xFF
            if fill["side"] == "sell":
                reasons.add(payload)
            else:
                masks.add(payload)
    assert reasons == {
        rule.RECOVER_TO_HIGH, rule.TAKE_PROFIT, rule.STOP_LOSS, rule.TRAILING_STOP,
        rule.TIME_STOP,
    }
    assert any(bin(m).count("1") > 1 for m in masks)
    assert rejected > 0


def test_warmup_bars_never_trade():
    case = next(c for c in CASES if c["name"].startswith("warmup"))
    start = dt.date.fromisoformat(case["start"])
    first_decision = next(i for i, d in enumerate(DATES) if d >= start)
    fills = EXPECTED[case["name"]]["fills"]
    assert fills
    assert all(f["bar"] > first_decision for f in fills)


def test_no_reentry_means_one_campaign():
    fills = EXPECTED["no re-entry after the first exit"]["fills"]
    assert [f["side"] for f in fills] == ["buy", "sell"]


def test_the_benchmark_buys_once_on_the_second_bar_and_holds():
    fills = EXPECTED["buy and hold, the benchmark"]["fills"]
    assert len(fills) == 1
    assert fills[0]["bar"] == 1 and fills[0]["side"] == "buy"


def test_every_fill_is_a_bar_after_its_decision():
    # Decisions happen at a close and fill at the next one, so no fill can be on
    # the first bar.
    for result in EXPECTED.values():
        assert all(f["bar"] >= 1 for f in result["fills"])


@pytest.mark.parametrize(
    "x, rounded",
    [
        (0.49999999999999994, 0),
        (0.5, 1),
        (2.5, 3),
        (-2.5, -3),
        (-0.5, -1),
        (1e15 + 0.5, 1_000_000_000_000_001),
    ],
)
def test_llround_is_half_away_from_zero_on_the_double(x, rounded):
    assert rule.llround(x) == rounded
