"""M8: the sweep machinery, and the threshold constant it produced.

The committed measurement is 1512 runs and takes the better part of an hour; a
test suite cannot contain it. What these tests cover is that the family of
variants is the size the spec asks for, that the statistics are computed the way
the report says they are, and -- the one that stops the numbers rotting -- that
the constant in `research/thresholds.py` still matches the committed sweep it
claims to come from.
"""

from __future__ import annotations

import json

import numpy as np
import pandas as pd
import pytest

from research import metrics, thresholds
from research.panel import PanelSpec
from research.sweep import (
    CONVENTIONAL_T,
    NULL_SPEC,
    RESULTS,
    best_of_n,
    expected_max_sharpe,
    false_discovery_rate,
    run_sweep,
    summarize,
    variants,
)


def test_the_family_is_at_least_the_size_the_spec_asks_for():
    configs = variants()
    assert len(configs) >= 200
    assert len({config.label for config in configs}) == len(configs)
    assert {config.model for config in configs} == {"ridge", "gbm"}
    assert {config.horizon for config in configs} == {1, 5, 20}


def test_no_variant_in_the_family_can_see_the_future():
    """A leaking variant in the null sweep would make the threshold meaningless."""
    for config in variants():
        assert all(not feature.leaks for feature in config.features), config.label


def test_a_panel_with_a_signal_is_refused():
    with pytest.raises(ValueError, match="requires lam = 0"):
        run_sweep(panels=1, spec=NULL_SPEC.replace(lam=0.001), configs=variants()[:1])


def test_the_null_panel_is_actually_null():
    """lam = 0 is necessary and not sufficient.

    A departure hazard tilted by the exposure puts a real relationship into a
    panel whose lam is zero: low-exposure names die more often, a departure is
    -30%, and the exposure predicts forward returns through that channel. The
    first version of NULL_SPEC had exactly that, and a false-discovery rate
    measured on it would have been inflated by a signal that was genuinely there.

    So this checks the panel the sweep actually uses, pooling several seeds to
    get an estimate tight enough to be worth checking.
    """
    from research.labels import forward_return
    from research.panel import generate_panel

    slopes = []
    for seed in (20260918, 20260919, 20260920, 20260921):
        panel = generate_panel(NULL_SPEC.replace(seed=seed))
        labels = forward_return(panel.frame, horizon=5)
        slopes.append(metrics.cross_sectional_slopes(panel.frame["exposure"], labels.values))

    pooled = pd.concat(slopes, ignore_index=True)
    estimate = metrics.LambdaEstimate(float(pooled.mean()),
                                      metrics.newey_west_se(pooled, 4),
                                      int(pooled.size), 4)
    assert estimate.covers(0.0, sigmas=2.5), (
        f"the null panel is not null: {estimate}. Check delist_exposure_tilt.")


def test_a_tilted_hazard_would_have_been_caught():
    """The control for the test above: with the tilt on, it fails.

    Two things have to be true at once now, and the second is new. The tilt has
    to leave a measurable trace -- otherwise the null check above proves only
    that the current panel passes it. And the trace only exists once the
    **delisting return is applied**: Session D moved it out of the price series,
    so under the default `last_price` a tilted hazard changes who leaves and
    costs nothing, which leaves the returns alone.

    That is a real improvement to the null and worth stating rather than
    absorbing: the sweep's panels are now null under the default model even if
    somebody re-tilts the hazard. This control therefore has to ask for the
    contamination explicitly.
    """
    from research.delisting import APPLY_TRUE_RETURN, applied_returns
    from research.labels import forward_return
    from research.panel import generate_panel

    slopes = []
    for seed in (20260918, 20260919, 20260920, 20260921):
        panel = generate_panel(NULL_SPEC.replace(seed=seed, delist_exposure_tilt=0.8,
                                                 delist_hazard=0.002))
        labels = forward_return(
            panel.frame, horizon=5,
            delisting_returns=applied_returns(panel.delistings, APPLY_TRUE_RETURN))
        slopes.append(metrics.cross_sectional_slopes(panel.frame["exposure"], labels.values))

    pooled = pd.concat(slopes, ignore_index=True)
    estimate = metrics.LambdaEstimate(float(pooled.mean()),
                                      metrics.newey_west_se(pooled, 4),
                                      int(pooled.size), 4)
    assert not estimate.covers(0.0, sigmas=2.5), (
        f"a hazard tilted at 0.8 left no measurable trace: {estimate}. The null check "
        f"above is not sensitive enough to protect the sweep.")


@pytest.fixture(scope="module")
def small_sweep():
    """Six variants over two small null panels. Enough to exercise every column.

    The first six of the family, deliberately, rather than a spread across it:
    they share feature sets and horizons while differing in model, which is what
    produces the duplicated lambda estimates the deduplication test needs to see.
    """
    configs = variants()[:6]
    spec = PanelSpec(n_securities=60, n_periods=400, lam=0.0, seed=0,
                     delist_hazard=0.0005, delist_exposure_tilt=0.0)
    return run_sweep(panels=2, spec=spec, configs=configs, verbose=False)


def test_a_sweep_produces_a_row_per_panel_and_variant(small_sweep):
    assert len(small_sweep) == 12
    assert small_sweep["panel"].nunique() == 2
    for column in ("ic_t", "lambda_t", "sharpe", "sharpe_t", "oos_r2", "turnover"):
        assert column in small_sweep.columns


def test_nothing_is_found_on_average(small_sweep):
    """Not an assertion about any one run -- the point of M8 is that individual
    runs do find things. The average across them has to be nothing."""
    assert abs(small_sweep["ic_mean"].mean()) < 0.02
    assert abs(small_sweep["sharpe"].mean()) < 1.5


def test_the_sharpe_t_statistic_is_the_sharpe_scaled_by_the_sample(small_sweep):
    """A Sharpe's t-statistic is what anybody comparing a Sharpe to 1.96 forms
    implicitly. If this drifts from the Sharpe, the false discovery rate for the
    Sharpe column is measuring something nobody asked for."""
    ratio = (small_sweep["sharpe_t"] / small_sweep["sharpe"]).dropna()
    # Not identical across runs: the factor is the square root of the sample in
    # years, and a twenty-period horizon labels fewer dates than a one-period
    # one, so a long-horizon variant is scaled by slightly less. The spread
    # between them is small; a large one would mean the scaling had come loose
    # from the sample length.
    assert ratio.std() < 0.15
    assert 1.0 < ratio.mean() < 2.0


def test_false_discovery_rate_is_a_fraction(small_sweep):
    rates = false_discovery_rate(small_sweep, CONVENTIONAL_T)
    for name, value in rates.items():
        if name.endswith("_n"):
            continue  # a trial count, not a rate
        assert 0.0 <= value <= 1.0, f"{name} = {value}"
    assert rates["ic_t"] >= rates["ic_t_positive"]


def test_the_lambda_rate_is_counted_over_distinct_estimates(small_sweep):
    """lambda_hat does not depend on a variant's features or model.

    It is measured against the panel's raw exposure, so it varies only with the
    panel, the horizon, and how far the feature windows warmed up. Counting it
    once per variant would report a rate on a denominator that is mostly copies
    of itself -- in the committed sweep, 1512 runs carrying 108 distinct
    estimates. The first version of this sweep did exactly that.
    """
    rates = false_discovery_rate(small_sweep, CONVENTIONAL_T)

    assert rates["lambda_t_n"] < rates["ic_t_n"], "no deduplication happened at all"
    assert rates["lambda_t_n"] == small_sweep["lambda_t"].round(9).nunique()

    # The duplication is real and worth asserting: several variants share an
    # estimate exactly, which is what makes the naive denominator wrong.
    counts = small_sweep.groupby("lambda_t").size()
    assert counts.max() > 1


def test_best_of_n_rises_with_n(small_sweep):
    """The whole argument of the milestone, as an assertion: trying more things
    raises the best result you find in nothing."""
    table = best_of_n(small_sweep, sizes=(1, 2, 4, 6), draws=2000, seed=1)
    assert table["mean"].is_monotonic_increasing
    assert table["p95"].is_monotonic_increasing or table["p95"].nunique() < len(table)


def test_best_of_n_is_deterministic(small_sweep):
    first = best_of_n(small_sweep, sizes=(1, 4), draws=500, seed=7)
    second = best_of_n(small_sweep, sizes=(1, 4), draws=500, seed=7)
    pd.testing.assert_frame_equal(first, second)


def test_the_analytic_expected_maximum_behaves():
    """A cross-check has to be checked. Known shape: rises with n, scales with
    sigma, undefined for a single draw."""
    assert expected_max_sharpe(1.0, 2) < expected_max_sharpe(1.0, 100)
    assert expected_max_sharpe(2.0, 100) == pytest.approx(2.0 * expected_max_sharpe(1.0, 100))
    assert not np.isfinite(expected_max_sharpe(1.0, 1))
    assert not np.isfinite(expected_max_sharpe(0.0, 10))
    # Standard normal, 100 draws: textbook value is a shade over 2.5.
    assert 2.3 < expected_max_sharpe(1.0, 100) < 2.8


def test_summarize_reports_everything_the_threshold_rests_on(small_sweep):
    summary = summarize(small_sweep)
    for key in ("runs", "panels", "variants", "sharpe_sd", "fdr_conventional",
                "best_of_n", "threshold", "expected_max_analytic"):
        assert key in summary
    assert summary["runs"] == len(small_sweep)


# ---------------------------------------------------------------------------
# The committed measurement
# ---------------------------------------------------------------------------


@pytest.mark.skipif(not RESULTS.exists(), reason="the committed sweep has not been run")
def test_the_published_threshold_matches_the_committed_sweep():
    """The constant and the run it came from cannot drift apart silently.

    A threshold nobody can trace to a measurement is a remembered number, which is
    exactly what the milestone exists to replace.
    """
    committed = json.loads(RESULTS.read_text(encoding="utf-8"))
    summary = committed["summary"]

    assert thresholds.NULL_SHARPE_THRESHOLD == pytest.approx(summary["threshold"], abs=5e-3)
    assert thresholds.NULL_SWEEP_VARIANTS == summary["variants"]
    assert thresholds.NULL_SWEEP_PANELS == summary["panels"]
    assert thresholds.NULL_SWEEP_RUNS == summary["runs"]


@pytest.mark.skipif(not RESULTS.exists(), reason="the committed sweep has not been run")
def test_the_committed_sweep_was_run_on_a_null_panel():
    committed = json.loads(RESULTS.read_text(encoding="utf-8"))
    assert float(committed["spec"]["lam"]) == 0.0


def test_the_threshold_helper_judges_against_the_size_of_the_search():
    """More variants, higher bar. The helper exists so that the adjustment is not
    left to whoever is reading the result."""
    modest = thresholds.distinguishable(1.5, variants_tried=10)
    searched = thresholds.distinguishable(1.5, variants_tried=250)
    assert modest and not searched

    assert thresholds.threshold_for(10) < thresholds.threshold_for(250)

    # The anchor is the *effective* try count, not the raw one. Asking for 252 --
    # the number of variants the sweep ran -- correctly returns more than the
    # measured threshold, because those 252 were measured to behave like 81
    # independent tries and a caller's 252 have not been.
    assert thresholds.threshold_for(thresholds.NULL_EFFECTIVE_TRIES) == pytest.approx(
        thresholds.NULL_SHARPE_THRESHOLD, abs=0.01)
    assert thresholds.threshold_for(thresholds.NULL_SWEEP_VARIANTS) > (
        thresholds.NULL_SHARPE_THRESHOLD)


def test_a_single_try_is_the_ordinary_quantile():
    """With one variant there is no multiple-comparison problem, and the bar is
    just the one-sided 95th percentile of the null Sharpe."""
    single = thresholds.threshold_for(1)
    assert single == pytest.approx(1.645 * thresholds.NULL_SHARPE_SD +
                                   thresholds.NULL_SHARPE_MEAN, abs=0.01)
    assert single < thresholds.threshold_for(10)
