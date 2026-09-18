"""Statistics for the research layer, and the estimator lam is measured with.

The convention of `include/marketscan/metrics/performance.hpp` holds here:
**undefined results are NaN, never zero.** A Sharpe ratio of zero is the claim
that a strategy earned nothing per unit of risk. A Sharpe ratio that could not be
computed is the absence of a claim. Collapsing the two is how an empty result set
comes to look like a flat one.

THE ESTIMATOR

`fama_macbeth` is the load-bearing function of M6 and M8. It fits the
cross-sectional slope of forward return on exposure separately on every date,
then treats the series of slopes as the sample:

    lam_hat = mean_t(slope_t),   se = sd(slope_t) / sqrt(T)

This is preferred over one pooled regression for a specific reason. A pooled fit
assumes the residuals are independent across the whole panel, and they are not:
every name on one date shares that date's market move, so a pooled standard error
is wrong by a factor that depends on N. The per-date slopes already have the
common component differenced out of them, and the only remaining dependence is
across time.

Which leaves overlap. With a label horizon of h > 1, consecutive slopes are
estimated from returns that share h-1 periods, so they are autocorrelated and the
naive standard error above is too small -- an inflated t-statistic, which is the
single most common way a research result is wrong rather than merely unlucky.
`nw_lags` applies a Newey-West correction; `fama_macbeth_for_horizon` sets it
from the horizon so the caller cannot forget.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd
from scipy import stats

UNDEFINED = float("nan")

ANNUAL_PERIODS = 252.0
"""Business days per year. The same 252 the C++ metrics use, so a Sharpe from
the research layer and a Sharpe from the engine are the same number."""


_RELATIVE_FLOOR = 1e-12
"""Below this fraction of a series' own scale, a spread is rounding, not risk.

A guard written as `sd <= 0.0` does not hold. The sample standard deviation of
thirty copies of 0.001 is not zero, it is about 1e-19, because the mean is
computed by summation and the residuals do not cancel exactly -- and dividing a
mean of 0.001 by 1e-19 produced a Sharpe ratio of 7.2e16, which is the precise
failure this module's docstring promises not to commit. Whether it happened
depended on the length of the series, so thirty observations produced it and
fifty did not.

The floor is relative because the alternative is an absolute one, and an absolute
threshold is wrong for at least one of daily returns and annualized ones.
"""


def _clean(values: pd.Series | np.ndarray) -> np.ndarray:
    array = np.asarray(values, dtype=float)
    return array[np.isfinite(array)]


def _degenerate(spread: float, sample: np.ndarray) -> bool:
    """Whether `spread` is indistinguishable from zero at this series' scale."""
    if not np.isfinite(spread) or spread <= 0.0:
        return True
    scale = float(np.max(np.abs(sample))) if sample.size else 0.0
    return spread <= _RELATIVE_FLOOR * max(scale, np.finfo(float).tiny)


def mean(returns: pd.Series | np.ndarray) -> float:
    clean = _clean(returns)
    return float(clean.mean()) if clean.size else UNDEFINED


def volatility(returns: pd.Series | np.ndarray, annualize: bool = True) -> float:
    """Sample standard deviation, dividing by n-1. NaN for fewer than two."""
    clean = _clean(returns)
    if clean.size < 2:
        return UNDEFINED
    sd = float(clean.std(ddof=1))
    if _degenerate(sd, clean):
        sd = 0.0  # a measurement of no variation, not an absence of one
    return sd * np.sqrt(ANNUAL_PERIODS) if annualize else sd


def sharpe(returns: pd.Series | np.ndarray, annualize: bool = True) -> float:
    """Mean over standard deviation. NaN when the series is constant.

    A constant series has no risk-adjusted return: the ratio's denominator is
    zero and the answer is not infinity, it is undefined.
    """
    clean = _clean(returns)
    if clean.size < 2:
        return UNDEFINED
    sd = float(clean.std(ddof=1))
    if _degenerate(sd, clean):
        return UNDEFINED
    ratio = float(clean.mean() / sd)
    return ratio * np.sqrt(ANNUAL_PERIODS) if annualize else ratio


def sortino(returns: pd.Series | np.ndarray, annualize: bool = True) -> float:
    """Mean over downside deviation. NaN when nothing was ever down."""
    clean = _clean(returns)
    if clean.size < 2:
        return UNDEFINED
    downside = clean[clean < 0.0]
    if downside.size == 0:
        return UNDEFINED
    deviation = float(np.sqrt(np.mean(np.square(downside))))
    if _degenerate(deviation, clean):
        return UNDEFINED
    ratio = float(clean.mean() / deviation)
    return ratio * np.sqrt(ANNUAL_PERIODS) if annualize else ratio


def max_drawdown(returns: pd.Series | np.ndarray) -> float:
    """Largest peak-to-trough fall of the compounded series, as a fraction.

    Positive. NaN for an empty series; zero for a series that never fell, which
    is a measurement and not an absence of one.
    """
    clean = _clean(returns)
    if clean.size == 0:
        return UNDEFINED
    equity = np.cumprod(1.0 + clean)
    peak = np.maximum.accumulate(equity)
    return float(np.max(1.0 - equity / peak))


def t_stat(values: pd.Series | np.ndarray) -> float:
    """t-statistic of the mean against zero. NaN when it cannot be formed."""
    clean = _clean(values)
    if clean.size < 2:
        return UNDEFINED
    se = float(clean.std(ddof=1) / np.sqrt(clean.size))
    if _degenerate(se, clean):
        return UNDEFINED
    return float(clean.mean() / se)


def newey_west_se(values: pd.Series | np.ndarray, lags: int) -> float:
    """Standard error of the mean, corrected for autocorrelation up to `lags`.

    Bartlett weights. `lags = 0` reduces to the plain standard error.

    Clamped at zero from below: a small sample with strong negative
    autocorrelation can produce a negative long-run variance estimate, and a
    negative variance reported as a standard error would be a number that looks
    like an answer.
    """
    clean = _clean(values)
    n = clean.size
    if n < 2:
        return UNDEFINED
    demeaned = clean - clean.mean()
    variance = float(np.dot(demeaned, demeaned) / n)
    for lag in range(1, min(lags, n - 1) + 1):
        weight = 1.0 - lag / (lags + 1.0)
        covariance = float(np.dot(demeaned[lag:], demeaned[:-lag]) / n)
        variance += 2.0 * weight * covariance
    if not np.isfinite(variance) or variance <= 0.0:
        return UNDEFINED
    se = float(np.sqrt(variance / n))
    return UNDEFINED if _degenerate(se, clean) else se


@dataclass(frozen=True)
class LambdaEstimate:
    """A measured signal strength, with the error bars M6 compares against."""

    estimate: float
    stderr: float
    n_periods: int
    nw_lags: int

    @property
    def t_stat(self) -> float:
        if not np.isfinite(self.estimate) or not np.isfinite(self.stderr) or self.stderr <= 0.0:
            return UNDEFINED
        return self.estimate / self.stderr

    @property
    def p_value(self) -> float:
        """Two-sided, against a null of no relationship."""
        t = self.t_stat
        if not np.isfinite(t) or self.n_periods < 2:
            return UNDEFINED
        return float(2.0 * stats.t.sf(abs(t), df=self.n_periods - 1))

    def covers(self, truth: float, sigmas: float = 2.0) -> bool:
        """Whether an interval this many standard errors wide contains `truth`.

        The test M6 makes. Phrased as coverage of the true value rather than
        significance of the estimate, because "the pipeline found something" and
        "the pipeline found what was planted" are different claims and only the
        second one is calibration.
        """
        if not np.isfinite(self.estimate) or not np.isfinite(self.stderr):
            return False
        return abs(self.estimate - truth) <= sigmas * self.stderr

    def __str__(self) -> str:
        return (f"lam_hat={self.estimate:+.6f} +/- {self.stderr:.6f} "
                f"(t={self.t_stat:+.2f}, T={self.n_periods}, nw={self.nw_lags})")


def cross_sectional_slopes(x: pd.Series, y: pd.Series, min_names: int = 10) -> pd.Series:
    """The slope of y on x, fitted per date. Index is the date.

    Both series carry a (date, security) MultiIndex. A date with fewer than
    `min_names` usable pairs is dropped rather than fitted: a slope from four
    points is not an observation of the cross-section, it is noise with a
    coefficient attached.
    """
    frame = pd.DataFrame({"x": x, "y": y}).dropna()
    if frame.empty:
        return pd.Series(dtype=float)

    slopes: dict[pd.Timestamp, float] = {}
    for date, group in frame.groupby(level=0, sort=True):
        if len(group) < min_names:
            continue
        xv = group["x"].to_numpy()
        yv = group["y"].to_numpy()
        centered = xv - xv.mean()
        denominator = float(np.dot(centered, centered))
        if denominator <= 0.0:
            continue
        slopes[date] = float(np.dot(centered, yv - yv.mean()) / denominator)
    return pd.Series(slopes, dtype=float).sort_index()


def fama_macbeth(x: pd.Series, y: pd.Series, nw_lags: int = 0,
                 min_names: int = 10) -> LambdaEstimate:
    """Estimates lam: the per-date cross-sectional slope of y on x, averaged.

    `x` is the exposure known on date t, `y` the forward return labelled at date
    t. Both MultiIndexed (date, security).
    """
    slopes = cross_sectional_slopes(x, y, min_names=min_names)
    if slopes.size < 2:
        return LambdaEstimate(UNDEFINED, UNDEFINED, int(slopes.size), nw_lags)
    stderr = (newey_west_se(slopes, nw_lags) if nw_lags > 0
              else float(slopes.std(ddof=1) / np.sqrt(slopes.size)))
    return LambdaEstimate(float(slopes.mean()), stderr, int(slopes.size), nw_lags)


def fama_macbeth_for_horizon(x: pd.Series, y: pd.Series, horizon: int,
                             min_names: int = 10) -> LambdaEstimate:
    """`fama_macbeth` with the overlap correction the horizon implies.

    A horizon of h means consecutive labels share h-1 periods of return, so the
    slope series is autocorrelated out to lag h-1 by construction. Setting the
    lag count here rather than at the call site is deliberate: every caller that
    had to remember it would eventually be a caller that forgot, and the failure
    is silent -- a t-statistic that is too large.
    """
    return fama_macbeth(x, y, nw_lags=max(horizon - 1, 0), min_names=min_names)


def information_coefficient(scores: pd.Series, forward: pd.Series,
                            min_names: int = 10) -> pd.Series:
    """Per-date rank correlation between a score and the forward return.

    Spearman rather than Pearson: a score is an ordering, the portfolio layer
    only ever uses the ordering, and a single outlying return should not be able
    to make a useless score look predictive.
    """
    frame = pd.DataFrame({"s": scores, "f": forward}).dropna()
    if frame.empty:
        return pd.Series(dtype=float)

    out: dict[pd.Timestamp, float] = {}
    for date, group in frame.groupby(level=0, sort=True):
        if len(group) < min_names:
            continue
        if group["s"].nunique() < 2 or group["f"].nunique() < 2:
            continue
        rho = stats.spearmanr(group["s"].to_numpy(), group["f"].to_numpy()).statistic
        if np.isfinite(rho):
            out[date] = float(rho)
    return pd.Series(out, dtype=float).sort_index()


def out_of_sample_r2(actual: pd.Series, predicted: pd.Series) -> float:
    """R-squared against a zero-prediction benchmark, not against the mean.

    1 - SSE/SST where SST is measured about zero. This is the out-of-sample
    convention: the alternative would be to credit a model for knowing the mean
    of the test period, which it did not know.

    Expect a small number. An honest daily cross-sectional forecast lands in the
    low single digits of a percent, and anything above a couple of percent is a
    reason to go looking for the leak -- which is precisely the threshold M7's
    lookahead test asserts.
    """
    frame = pd.DataFrame({"a": actual, "p": predicted}).dropna()
    if frame.empty:
        return UNDEFINED
    a = frame["a"].to_numpy()
    p = frame["p"].to_numpy()
    sst = float(np.dot(a, a))
    if sst <= 0.0:
        return UNDEFINED
    residual = a - p
    return float(1.0 - np.dot(residual, residual) / sst)


def summarize(returns: pd.Series) -> dict[str, float]:
    """The numbers a result is reported with. Every one of them can be NaN."""
    return {
        "mean": mean(returns) * ANNUAL_PERIODS if np.isfinite(mean(returns)) else UNDEFINED,
        "volatility": volatility(returns),
        "sharpe": sharpe(returns),
        "sortino": sortino(returns),
        "max_drawdown": max_drawdown(returns),
        "periods": int(_clean(returns).size),
    }
