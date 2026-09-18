"""The measured bar a result has to clear, importable.

Spec 2 M8 asks for the threshold to be "exposed as a constant the research code
can import, so that future results are judged against a measured bar rather than
a remembered one". This is that constant.

The numbers below are **measurements**, not calculations. They were produced by
`python -m research.sweep`, they are recorded in `docs/figures/false_discovery.json`
alongside every run they came from, and `research/tests/test_sweep.py` asserts
that this file and that file still agree. Editing a number here without re-running
the sweep breaks that test, which is the intent: a threshold nobody can trace to a
measurement is exactly the remembered number the milestone exists to replace.

HOW TO USE IT

    from research.thresholds import distinguishable, threshold_for

    if not distinguishable(result.sharpe, variants_tried=40):
        ...

Count the variants honestly. Every configuration tried counts, including the ones
abandoned after a glance, and including the ones tried on a previous day. The bar
is a function of how many things were looked at, not of how many were written
down.
"""

from __future__ import annotations

import numpy as np
from scipy import stats

# ---------------------------------------------------------------------------
# Measured by research/sweep.py. See docs/FALSE_DISCOVERY.md.
# ---------------------------------------------------------------------------

MEASURED_ON = "2026-09-18"

NULL_SHARPE_SKEW = -0.262
NULL_SHARPE_KURTOSIS = 0.064
"""Close enough to normal that the extreme-value machinery below is legitimate."""

NULL_SWEEP_VARIANTS = 252
"""Pipeline variants in the sweep: 21 feature sets x 3 horizons x 4 models."""

NULL_SWEEP_PANELS = 6
"""Independently seeded synthetic panels, each with lambda = 0."""

NULL_SWEEP_RUNS = 1512

NULL_SHARPE_SD = 0.5785
"""Standard deviation of the annualized Sharpe across all 1512 null runs."""

NULL_SHARPE_MEAN = 0.0115
"""And its mean, which is zero to three decimal places. The first thing to check:
a null sweep with a positive average Sharpe is not a null sweep."""

NULL_EFFECTIVE_TRIES = 81
"""What 252 correlated variants are worth in independent ones.

Calibrated from the observed mean best-of-252. Variants sharing
a feature, a horizon or a model are not separate tries, and treating them as
though they were would set the bar higher than the evidence supports."""

NULL_SHARPE_THRESHOLD = 1.962
"""**The bar.** 95th percentile of the best-of-252 Sharpe under
the null.

A search of this size, over three years of data, produces a best variant at or
above this **one time in twenty when nothing is there at all**. See
docs/FALSE_DISCOVERY.md, which does not soften it."""

EULER_MASCHERONI = 0.5772156649015329


def threshold_for(variants_tried: int, percentile: float = 0.95) -> float:
    """The bar for a search of a different size.

    `variants_tried` is counted as **independent** tries, and the anchor is the
    measured threshold at `NULL_EFFECTIVE_TRIES`. So `threshold_for(81)` returns
    the measured number and `threshold_for(252)` returns something higher --
    which is correct rather than inconsistent: the sweep's own 252 variants were
    measured to behave like 81 independent ones, and a caller's 252 variants have
    a correlation nobody has measured. Assuming independence is the conservative
    direction for them.

    The scaling between try counts comes from the extreme-value formula, which is
    the part of the shape that is known analytically. The anchor carries what the
    formula cannot: the measured spread of this pipeline's null Sharpes.

    For a single variant the extreme-value formula is undefined, and the honest
    answer is the ordinary one-sided quantile of the null Sharpe distribution.
    """
    if variants_tried < 1:
        raise ValueError("variants_tried must be at least one")
    if not np.isfinite(NULL_SHARPE_THRESHOLD):
        raise RuntimeError("the threshold has not been measured; run python -m research.sweep")

    if variants_tried == 1:
        return float(stats.norm.ppf(percentile) * NULL_SHARPE_SD + NULL_SHARPE_MEAN)

    # Scaled through the *effective* try count, not the raw one: the anchor was
    # calibrated on 252 variants that behave like NULL_EFFECTIVE_TRIES
    # independent ones, and a caller asking about 40 variants of their own is
    # asking about 40 tries whose correlation nobody has measured. Assuming they
    # are independent is the conservative direction for them.
    anchor = _expected_max(NULL_EFFECTIVE_TRIES)
    target = _expected_max(variants_tried)
    return float(NULL_SHARPE_THRESHOLD * target / anchor)


def distinguishable(sharpe: float, variants_tried: int) -> bool:
    """Whether a Sharpe is distinguishable from the best of a search this size.

    False does not mean the result is false. It means it is inside the range a
    search over data containing nothing produces as a matter of course, which is
    the same thing as far as a decision is concerned.
    """
    if not np.isfinite(sharpe):
        return False
    return sharpe > threshold_for(variants_tried)


def _expected_max(n: int) -> float:
    """Expected maximum of n standard normal draws. Scale-free, so the units of
    the anchor carry through."""
    upper = stats.norm.ppf(1.0 - 1.0 / n)
    lower = stats.norm.ppf(1.0 - 1.0 / (n * np.e))
    return float((1.0 - EULER_MASCHERONI) * upper + EULER_MASCHERONI * lower)
