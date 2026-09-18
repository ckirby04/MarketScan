"""M6: the pipeline recovers a planted lam across a sweep, including small ones.

A smaller sweep than the committed figure -- fewer periods, one seed -- because a
test is a gate rather than a result. The figure in `docs/figures/` is the result,
and `python -m research.recovery` regenerates it.
"""

from __future__ import annotations

import numpy as np
import pytest

from research.panel import PanelSpec
from research.pipeline import PipelineConfig
from research.recovery import DEFAULT_LAMS, aggregate, plot_recovery, recovery_sweep


@pytest.fixture(scope="module")
def sweep():
    return recovery_sweep(
        lams=DEFAULT_LAMS,
        seeds=(20260918,),
        spec=PanelSpec(n_securities=150, n_periods=900),
        config=PipelineConfig(build_portfolio=False),
    )


def test_every_lam_is_recovered_within_two_standard_errors(sweep):
    """M6's definition of done, across six values including zero."""
    assert len(sweep) == len(DEFAULT_LAMS) >= 5
    missed = sweep.loc[~sweep["covers"]]
    assert missed.empty, f"missed: {missed[['lam', 'estimate', 'stderr']].to_dict('records')}"


def test_the_recovery_is_a_straight_line_through_the_origin(sweep):
    """Coverage alone is weak: wide enough error bars cover anything.

    A slope of one and an intercept of zero is the stronger claim, and it is the
    one the figure makes visually.
    """
    truth = sweep["lam"].to_numpy()
    estimate = sweep["estimate"].to_numpy()
    slope, intercept = np.polyfit(truth, estimate, 1)

    assert slope == pytest.approx(1.0, abs=0.05)
    assert abs(intercept) < 2.0 * float(sweep["stderr"].mean())


def test_a_smaller_lam_is_harder_to_see(sweep):
    """The information coefficient rises with lam -- above the noise floor.

    Monotone across *every* lam is the wrong assertion and it failed the first
    time it was run: at lam = 0 and lam = 1bp the measured IC is 0.0046 and
    0.0016, and both of those are one standard error of nothing. Ordering two
    numbers that are indistinguishable from zero and from each other is not a
    property of the pipeline, it is a property of one seed.

    So: monotone where the signal is detectable, and indistinguishable from zero
    where it is not. That is the real claim, and it still catches the failure
    that matters -- an IC that does not depend on what was planted.
    """
    summary = aggregate(sweep).sort_values("lam")

    detectable = summary[summary["lam"] >= 0.00025]
    assert detectable["ic_mean"].is_monotonic_increasing

    floor = summary[summary["lam"] <= 0.0001]
    assert (floor["ic_mean"].abs() < detectable["ic_mean"].min()).all()


def test_zero_is_recovered_as_zero(sweep):
    null = sweep.loc[sweep["lam"] == 0.0].iloc[0]
    assert abs(null["estimate"]) < 2.0 * null["stderr"]


def test_a_churning_panel_is_refused(sweep):
    """Recovery on a panel with a tilted hazard would be one bias cancelling
    another. The sweep refuses rather than quietly reporting it."""
    with pytest.raises(ValueError, match="without churn"):
        recovery_sweep(lams=(0.001,), seeds=(1,),
                       spec=PanelSpec(n_securities=20, n_periods=100, delist_hazard=0.01))


def test_the_plot_is_produced(sweep, tmp_path):
    path = plot_recovery(sweep, tmp_path / "recovery.png")
    assert path.exists()
    assert path.stat().st_size > 10_000
