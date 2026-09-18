"""M8b: the information coefficient bar, and the constant it produced.

The committed calibration is 1512 runs across six horizons. What is tested here
is the machinery, the shape of the answer, and -- the part that stops the numbers
rotting -- that `research/thresholds.py` still agrees with the committed sweep.
"""

from __future__ import annotations

import json

import numpy as np
import pandas as pd
import pytest

from research import thresholds
from research.ic_calibration import IC_HORIZONS, RESULTS, ic_thresholds, ic_variants
from research.panel import PanelSpec
from research.sweep import run_sweep


def test_the_family_spans_the_horizons_and_builds_no_books():
    configs = ic_variants()
    assert {config.horizon for config in configs} == set(IC_HORIZONS)
    assert all(not config.build_portfolio for config in configs), (
        "the IC does not need a portfolio, and building one triples the sweep")
    assert all(not feature.leaks for config in configs for feature in config.features)


@pytest.fixture(scope="module")
def small_sweep():
    """Two horizons over two small null panels."""
    configs = [config for config in ic_variants((1, 20))
               if config.features[0].source in ("exposure", "ret")][:12]
    spec = PanelSpec(n_securities=60, n_periods=400, lam=0.0, seed=0,
                     delist_hazard=0.0005, delist_exposure_tilt=0.0)
    return run_sweep(panels=2, spec=spec, configs=configs, verbose=False)


def test_a_row_per_horizon_with_every_column_the_report_needs(small_sweep):
    table = ic_thresholds(small_sweep)
    assert set(table["horizon"]) == {1, 20}
    for column in ("ic_sd", "nominal_ic", "t_false_positive", "single_test",
                   "mean_best", "n_effective", "best_of_n"):
        assert column in table.columns
        assert table[column].notna().all()


def test_the_bar_rises_with_the_horizon(small_sweep):
    """A longer label means a noisier per-date rank correlation, so the bar an
    honest result has to clear rises. If it did not, the horizon would not be
    the thing driving the miscalibration."""
    table = ic_thresholds(small_sweep).sort_values("horizon")
    assert table["ic_sd"].is_monotonic_increasing
    assert table["single_test"].is_monotonic_increasing


def test_searching_costs_more_than_a_single_test(small_sweep):
    table = ic_thresholds(small_sweep)
    assert (table["best_of_n"] > table["single_test"]).all()


def test_the_calibration_is_not_pinned_at_its_search_bound(small_sweep):
    """The effective-tries search runs to twice the variant count, because
    maximizing |IC| over n variants behaves like maximizing a signed IC over 2n.

    Searching only to n pinned four of six horizons at the ceiling in the first
    committed run, which is a bound rather than an answer.
    """
    table = ic_thresholds(small_sweep)
    assert (table["n_effective"] <= 2 * table["variants"]).all()
    assert (table["n_effective"] > 1).all()


# ---------------------------------------------------------------------------
# The exported constants
# ---------------------------------------------------------------------------


def test_the_exported_bars_match_the_committed_sweep():
    """The constant and the measurement it claims to come from cannot drift."""
    if not RESULTS.exists():
        pytest.skip("the committed IC calibration has not been run")

    committed = json.loads(RESULTS.read_text(encoding="utf-8"))["summary"]
    assert thresholds.IC_SWEEP_RUNS == committed["runs"]
    assert thresholds.IC_SWEEP_PANELS == committed["panels"]

    for row in committed["by_horizon"]:
        bar = thresholds.IC_BARS[int(row["horizon"])]
        assert bar.single_test == pytest.approx(row["single_test"], abs=1e-5)
        assert bar.best_of_n == pytest.approx(row["best_of_n"], abs=1e-5)
        assert bar.t_false_positive == pytest.approx(row["t_false_positive"], abs=1e-4)


def test_the_nominal_rule_is_anti_conservative_at_every_measured_horizon():
    """The finding, as an assertion. If a future change made the t-statistic
    honest, this fails and the documentation around it needs rewriting -- which
    is the correct outcome, not a nuisance."""
    for horizon, bar in thresholds.IC_BARS.items():
        assert bar.t_false_positive > 0.05, horizon
        assert bar.single_test > bar.nominal, horizon


def test_the_measured_rate_is_available_at_the_call_site():
    from research.panel import generate_panel
    from research.pipeline import PipelineConfig, run_pipeline

    panel = generate_panel(PanelSpec(n_securities=40, n_periods=300, lam=0.001, seed=3))
    result = run_pipeline(panel, PipelineConfig(build_portfolio=False))

    assert result.ic_t_false_positive == pytest.approx(
        thresholds.ic_t_false_positive_rate(1))
    assert result.ic_bar() == pytest.approx(thresholds.ic_threshold_for(1))
    assert result.ic_is_distinguishable() == thresholds.ic_distinguishable(
        result.ic_mean, 1)

    # And it is in the text a human reads, not only in the object.
    report = result.report()
    assert "fires on nothing" in report
    assert "measured bar" in report


def test_the_threshold_helper_mirrors_the_sharpe_one():
    single = thresholds.ic_threshold_for(20)
    searched = thresholds.ic_threshold_for(20, variants_tried=250)
    assert searched > single

    assert thresholds.ic_distinguishable(single * 1.1, 20)
    assert not thresholds.ic_distinguishable(single * 0.9, 20)
    # Absolute: the same magnitude with the other sign is the same finding.
    assert thresholds.ic_distinguishable(-single * 1.1, 20)


def test_an_uncalibrated_horizon_is_refused_rather_than_extrapolated():
    with pytest.raises(ValueError, match="outside the calibrated range"):
        thresholds.ic_threshold_for(max(thresholds.IC_HORIZONS) + 1)
    with pytest.raises(ValueError):
        thresholds.ic_t_false_positive_rate(0)

    # In between, it interpolates rather than refusing.
    middle = thresholds.ic_threshold_for(15)
    assert thresholds.ic_threshold_for(10) < middle < thresholds.ic_threshold_for(20)


def test_a_pipeline_at_an_uncalibrated_horizon_says_so_instead_of_guessing():
    from research.panel import generate_panel
    from research.pipeline import PipelineConfig, run_pipeline

    panel = generate_panel(PanelSpec(n_securities=40, n_periods=600, lam=0.0, seed=4))
    result = run_pipeline(panel, PipelineConfig(horizon=50, embargo=50,
                                                build_portfolio=False))
    assert np.isnan(result.ic_t_false_positive)
    assert np.isnan(result.ic_bar())
    assert result.ic_is_distinguishable() is False
