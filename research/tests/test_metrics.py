"""The statistics themselves, tested directly rather than through the pipeline.

Two things are being protected here. The NaN discipline -- an undefined result is
never zero, the same rule `include/marketscan/metrics/performance.hpp` follows --
and the estimator M6 and M8 both rest on. A Fama-MacBeth slope that is quietly
biased, or a standard error that is quietly too small, would make every other
result in `research/` wrong in a way no end-to-end test would catch, because the
end-to-end tests are calibrated against this estimator.
"""

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from research import metrics


# ---------------------------------------------------------------------------
# Undefined is NaN, never zero
# ---------------------------------------------------------------------------


def test_an_empty_series_has_no_statistics():
    empty = pd.Series(dtype=float)
    for function in (metrics.mean, metrics.volatility, metrics.sharpe, metrics.sortino,
                     metrics.max_drawdown, metrics.t_stat):
        assert np.isnan(function(empty)), function.__name__


def test_one_observation_is_not_a_sample():
    one = pd.Series([0.01])
    assert metrics.mean(one) == 0.01
    assert np.isnan(metrics.volatility(one))
    assert np.isnan(metrics.sharpe(one))
    assert np.isnan(metrics.t_stat(one))
    # A drawdown is defined for one observation: it either fell or it did not.
    assert metrics.max_drawdown(one) == 0.0


def test_a_constant_series_has_no_sharpe():
    """Not infinity, and emphatically not zero: the ratio is undefined."""
    flat = pd.Series([0.001] * 50)
    assert np.isnan(metrics.sharpe(flat))
    assert np.isnan(metrics.sortino(flat)), "a series that never fell has no downside"
    assert metrics.max_drawdown(flat) == 0.0


def test_non_finite_observations_are_ignored_not_propagated():
    clean = pd.Series([0.01, -0.02, 0.015, 0.005])
    dirty = pd.Series([0.01, np.nan, -0.02, 0.015, np.inf, 0.005])
    assert metrics.sharpe(dirty) == pytest.approx(metrics.sharpe(clean))


def test_sharpe_is_annualized_from_the_periodic_ratio():
    rng = np.random.default_rng(0)
    returns = pd.Series(rng.normal(0.001, 0.01, size=2000))

    periodic = metrics.sharpe(returns, annualize=False)
    annual = metrics.sharpe(returns)
    assert annual == pytest.approx(periodic * np.sqrt(metrics.ANNUAL_PERIODS))
    assert periodic == pytest.approx(0.1, abs=0.03)


def test_max_drawdown_is_the_peak_to_trough_fall():
    # +10%, then -50%, then +20%: peak 1.10, trough 0.55, so 50%.
    returns = pd.Series([0.10, -0.50, 0.20])
    assert metrics.max_drawdown(returns) == pytest.approx(0.5)


# ---------------------------------------------------------------------------
# Newey-West
# ---------------------------------------------------------------------------


def test_zero_lags_is_the_plain_standard_error():
    rng = np.random.default_rng(1)
    values = pd.Series(rng.normal(0.0, 1.0, size=500))
    plain = float(values.std(ddof=0) / np.sqrt(len(values)))
    assert metrics.newey_west_se(values, 0) == pytest.approx(plain)


def test_positive_autocorrelation_widens_the_standard_error():
    """The correction has to do something, in the right direction.

    Overlapping labels produce positively autocorrelated slopes, and the whole
    reason the correction exists is that the uncorrected error is too small --
    which is the single most common way a research result is wrong rather than
    merely unlucky.
    """
    rng = np.random.default_rng(2)
    shocks = rng.normal(size=4000)
    # A moving average of five: exactly the structure a five-period overlapping
    # label induces in a series of per-date slopes.
    overlapping = pd.Series(np.convolve(shocks, np.ones(5) / 5.0, mode="valid"))

    naive = metrics.newey_west_se(overlapping, 0)
    corrected = metrics.newey_west_se(overlapping, 4)
    assert corrected > naive * 1.5


def test_a_negative_long_run_variance_is_not_reported_as_a_standard_error():
    """A short, strongly negatively autocorrelated sample can produce one."""
    alternating = pd.Series([1.0, -1.0] * 3)
    result = metrics.newey_west_se(alternating, 4)
    assert np.isnan(result) or result >= 0.0


# ---------------------------------------------------------------------------
# The estimator
# ---------------------------------------------------------------------------


def _panel(slope: float, n_dates: int = 400, n_names: int = 60, noise: float = 0.02,
           seed: int = 3) -> tuple[pd.Series, pd.Series]:
    """A synthetic cross-section with a known slope and nothing else in it."""
    rng = np.random.default_rng(seed)
    dates = pd.bdate_range("2010-01-04", periods=n_dates)
    names = [f"N{i:03d}" for i in range(n_names)]
    index = pd.MultiIndex.from_product([dates, names], names=["date", "security"])

    x = rng.standard_normal(len(index))
    common = np.repeat(rng.normal(0.0, 0.01, size=n_dates), n_names)
    y = slope * x + common + rng.normal(0.0, noise, size=len(index))
    return pd.Series(x, index=index), pd.Series(y, index=index)


@pytest.mark.parametrize("slope", [0.0, 0.001, 0.005])
def test_fama_macbeth_recovers_a_known_slope(slope):
    x, y = _panel(slope)
    estimate = metrics.fama_macbeth(x, y)
    assert estimate.covers(slope, sigmas=3.0), str(estimate)
    assert estimate.n_periods == 400


def test_the_common_component_does_not_enter_the_estimate():
    """Every name on a date shares that date's move, which is exactly why the
    estimator is per-date. A pooled regression would let that common component
    into the standard error and report a t-statistic that is too large."""
    x, y = _panel(0.001, noise=0.001)
    louder = y + pd.Series(
        np.repeat(np.random.default_rng(9).normal(0.0, 0.05, size=400), 60), index=y.index)

    quiet = metrics.fama_macbeth(x, y)
    loud = metrics.fama_macbeth(x, louder)

    assert loud.estimate == pytest.approx(quiet.estimate, abs=1e-9)
    assert loud.stderr == pytest.approx(quiet.stderr, abs=1e-9)


def test_a_thin_cross_section_is_dropped_rather_than_fitted():
    x, y = _panel(0.001, n_dates=50, n_names=40)
    full = metrics.fama_macbeth(x, y, min_names=10)
    assert full.n_periods == 50

    strict = metrics.fama_macbeth(x, y, min_names=41)
    assert strict.n_periods == 0
    assert np.isnan(strict.estimate)


def test_the_horizon_sets_the_lag_count_so_a_caller_cannot_forget():
    x, y = _panel(0.001)
    assert metrics.fama_macbeth_for_horizon(x, y, horizon=1).nw_lags == 0
    assert metrics.fama_macbeth_for_horizon(x, y, horizon=10).nw_lags == 9


def test_coverage_is_a_claim_about_the_truth_not_about_significance():
    """`covers` asks whether the interval contains the planted value. An estimate
    can be wildly significant and still not cover, which is the failure M6 is
    looking for."""
    estimate = metrics.LambdaEstimate(estimate=0.005, stderr=0.0001, n_periods=500, nw_lags=0)
    assert estimate.t_stat == pytest.approx(50.0)
    assert estimate.p_value < 1e-6
    assert estimate.covers(0.005)
    assert not estimate.covers(0.001)


def test_an_undefined_estimate_covers_nothing():
    undefined = metrics.LambdaEstimate(np.nan, np.nan, 0, 0)
    assert not undefined.covers(0.0)
    assert np.isnan(undefined.t_stat)
    assert np.isnan(undefined.p_value)


# ---------------------------------------------------------------------------
# Information coefficient and out-of-sample R-squared
# ---------------------------------------------------------------------------


def test_the_information_coefficient_is_a_rank_correlation():
    x, y = _panel(0.01, noise=0.001)
    ic = metrics.information_coefficient(x, y)
    assert len(ic) == 400
    assert ic.mean() > 0.9

    # Monotone but wildly nonlinear: a rank correlation does not care.
    assert metrics.information_coefficient(x, np.exp(y * 50)).mean() == pytest.approx(
        ic.mean(), abs=1e-9)


def test_a_constant_score_has_no_information_coefficient():
    x, y = _panel(0.001)
    flat = pd.Series(1.0, index=x.index)
    assert metrics.information_coefficient(flat, y).empty


def test_out_of_sample_r2_is_measured_about_zero():
    """Not about the test mean. A model is not credited for knowing a mean it
    could not have known."""
    actual = pd.Series([0.01, -0.01, 0.02, -0.02])

    perfect = metrics.out_of_sample_r2(actual, actual)
    assert perfect == pytest.approx(1.0)

    nothing = metrics.out_of_sample_r2(actual, pd.Series(0.0, index=actual.index))
    assert nothing == pytest.approx(0.0)

    # Predicting the sample mean of a zero-mean series is the same as predicting
    # zero; predicting a constant that is wrong scores below zero.
    biased = metrics.out_of_sample_r2(actual, pd.Series(0.05, index=actual.index))
    assert biased < 0.0


def test_summarize_reports_undefined_rather_than_zero():
    summary = metrics.summarize(pd.Series([0.001] * 30))
    assert np.isnan(summary["sharpe"])
    assert np.isnan(summary["sortino"])
    assert summary["periods"] == 30
