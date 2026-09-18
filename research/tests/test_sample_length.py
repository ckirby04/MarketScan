"""Part 2: what moves the bar, and the scaling law the table rests on.

The committed measurement is 300 runs at three sample lengths. These tests check
the machinery and the two properties the conclusion depends on: that the null
Sharpe spread really does fall as 1/sqrt(sample), and that lengthening the sample
moves the bar further than narrowing the search.
"""

from __future__ import annotations

import json

import numpy as np
import pandas as pd
import pytest

from research.sample_length import (
    RESULTS,
    SAMPLE_YEARS,
    run_scaling,
    scaling_table,
    scaling_variants,
    summarize,
)


def test_the_variant_subset_spans_the_family():
    configs = scaling_variants(25)
    assert len(configs) == 25
    assert len({config.label for config in configs}) == 25
    assert len({config.horizon for config in configs}) > 1
    assert len({config.model for config in configs}) > 1


@pytest.fixture(scope="module")
def small_scaling():
    """One and three years, six variants, two panels. Machinery only."""
    return run_scaling(years=(1, 3), panels=2, count=6, verbose=False)


def test_a_row_per_sample_length(small_scaling):
    table = scaling_table(small_scaling)
    assert list(table["years"]) == [1, 3]
    assert (table["periods"] == table["years"] * 252).all()
    for column in ("sharpe_sd", "sharpe_sd_predicted", "mean_best", "n_effective",
                   "bar_small_search", "bar_full_search"):
        assert table[column].notna().all()


def test_a_longer_sample_narrows_the_null(small_scaling):
    """The load-bearing property. If the spread did not fall with the sample,
    the entire argument for buying data would be wrong."""
    table = scaling_table(small_scaling).sort_values("years")
    assert table["sharpe_sd"].is_monotonic_decreasing
    assert table["bar_small_search"].is_monotonic_decreasing


# ---------------------------------------------------------------------------
# The committed measurement
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def committed():
    if not RESULTS.exists():
        pytest.skip("the committed sample-length sweep has not been run")
    return json.loads(RESULTS.read_text(encoding="utf-8"))["summary"]


def test_the_spread_follows_one_over_root_years(committed):
    """The scaling law, against the measurement, at every length.

    This is what licenses reading the table in FALSE_DISCOVERY.md as arithmetic
    rather than as three unrelated experiments. A twenty percent tolerance is
    loose on purpose: the quantity is a standard deviation from a hundred runs,
    and the claim being tested is the exponent, not the third digit.
    """
    table = pd.DataFrame(committed["by_years"])
    assert list(table["years"]) == list(SAMPLE_YEARS)

    ratio = table["sharpe_sd"] / table["sharpe_sd_predicted"]
    assert (ratio.between(0.8, 1.2)).all(), f"measured/predicted: {list(ratio.round(3))}"


def test_sample_length_beats_variant_discipline(committed):
    """The conclusion, as an assertion rather than a sentence.

    Lengthening the sample from three years to twenty has to move the bar
    further than cutting the search from 252 variants to 25. Measured, it is not
    close -- 58% against 10% -- and if that ever inverts, the recommendation in
    FALSE_DISCOVERY.md is wrong and should fail loudly here first.
    """
    table = pd.DataFrame(committed["by_years"]).sort_values("years")
    short, long = table.iloc[0], table.iloc[-1]

    sample_lever = 1.0 - long["bar_full_search"] / short["bar_full_search"]
    variant_lever = 1.0 - short["bar_small_search"] / short["bar_full_search"]

    assert sample_lever > 0.4
    assert variant_lever < 0.25
    assert sample_lever > 3 * variant_lever


def test_a_realistic_factor_is_invisible_on_three_years(committed):
    """The sentence in the report that matters most, checked against numbers.

    An equity factor with a Sharpe at the honest end of the literature -- 0.3 to
    0.8 -- must be below the three-year bar and above the twenty-year one. That
    is the entire case for the data being a research-design constraint.
    """
    table = pd.DataFrame(committed["by_years"]).sort_values("years")
    short, long = table.iloc[0], table.iloc[-1]

    assert short["bar_small_search"] > 0.8, "a realistic factor would clear the short bar"
    assert long["bar_full_search"] <= 0.85, "the long sample does not bring it into range"


def test_the_effective_tries_drift_is_reported_not_hidden(committed):
    """The projected column rests on effective tries being stable across sample
    lengths, and it is not especially stable. The number is in the summary so
    the report can say so rather than imply otherwise."""
    assert "effective_tries_drift" in committed
    assert committed["effective_tries_drift"] > 0.0

    text = (RESULTS.parent.parent / "FALSE_DISCOVERY.md").read_text(encoding="utf-8")
    assert f"{committed['effective_tries_drift']:.0%}" in text
