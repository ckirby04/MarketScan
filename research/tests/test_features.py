"""M5: no transform in `features.py` can see the future, except the one named for it.

The central test here is `test_no_feature_moves_when_the_future_changes`, which
is a structural check rather than a check of any one transform: it rewrites
tomorrow and asserts that every feature value on every earlier date is bit-for-
bit unchanged. A new transform added to the registry is covered by it
automatically, which is the point -- a point-in-time guarantee that has to be
re-established by hand for each new feature is one that will eventually be
forgotten.
"""

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from research.features import (
    TRANSFORMS,
    Feature,
    build_features,
    cross_sectional_rank,
    cross_sectional_zscore,
    exposure_feature,
    leaking,
)
from research.panel import PanelSpec, generate_panel


@pytest.fixture(scope="module")
def panel():
    return generate_panel(PanelSpec(n_securities=30, n_periods=120, seed=44))


def _all_causal_features() -> list[Feature]:
    return [Feature(source="ret", transform=name, window=5)
            for name in sorted(TRANSFORMS)
            if name != "leak"]


def test_every_registered_transform_builds(panel):
    built = build_features(panel.frame, _all_causal_features())
    assert len(built.columns) == len(TRANSFORMS) - 1
    assert built.notna().any().all(), "a transform produced nothing but NaN"


@pytest.mark.parametrize("transform", sorted(name for name in TRANSFORMS if name != "leak"))
def test_no_feature_moves_when_the_future_changes(panel, transform):
    """Perturb the last date; assert nothing before it moves.

    This is the whole of point-in-time safety on the feature side. If a transform
    reaches forward -- a centred window, a full-sample standardization, a
    backwards shift -- this test fails, and it fails loudly rather than producing
    a slightly better backtest.
    """
    feature = Feature(source="ret", transform=transform, window=5, cross_section="zscore")

    original = build_features(panel.frame, [feature])

    tampered = panel.frame.copy()
    last_date = panel.dates[-1]
    tampered.loc[last_date, "ret"] = tampered.loc[last_date, "ret"] + 1.0
    perturbed = build_features(tampered, [feature])

    before = original.loc[original.index.get_level_values(0) < last_date]
    after = perturbed.loc[perturbed.index.get_level_values(0) < last_date]
    pd.testing.assert_frame_equal(before, after)


def test_the_leak_transform_does_exactly_what_it_says(panel):
    """The pathology, confirmed to be a pathology. M7 depends on this working."""
    feature = Feature(source="ret", transform="leak", window=1, cross_section="none")
    built = build_features(panel.frame, [feature])

    last_date = panel.dates[-1]
    tampered = panel.frame.copy()
    tampered.loc[last_date, "ret"] = tampered.loc[last_date, "ret"] + 1.0
    perturbed = build_features(tampered, [feature])

    before = built.loc[built.index.get_level_values(0) < last_date]
    after = perturbed.loc[perturbed.index.get_level_values(0) < last_date]
    with pytest.raises(AssertionError):
        pd.testing.assert_frame_equal(before, after)

    assert feature.leaks
    assert leaking([feature]) == [feature.label]
    assert leaking([exposure_feature()]) == []


def test_a_rolling_window_never_spans_a_name_that_was_not_there(panel):
    """A window is per security, and only over dates the security existed.

    Without this, a 20-day mean for a name that joined five days ago is a mean of
    five of its own observations and fifteen of nothing -- or, worse, of whatever
    the unstacked matrix happened to leave in those cells.
    """
    churning = generate_panel(PanelSpec(n_securities=30, n_periods=200, seed=45,
                                        delist_hazard=0.003))
    window = 20
    built = build_features(churning.frame, [Feature(source="ret", transform="mean",
                                                    window=window, cross_section="none")])
    label = "ret.mean20.none"

    starts = churning.membership.set_index("security")["start"]
    checked = built[label].dropna()
    assert not checked.empty, "every value was NaN, so nothing below is checked"

    for (date, security), value in checked.items():
        elapsed = len(pd.bdate_range(starts[security], date))
        assert elapsed >= window, (f"{security} had a {window}-period mean after {elapsed} "
                                   f"periods of existence")


def test_cross_sectional_rank_is_centred_and_bounded():
    wide = pd.DataFrame([[1.0, 2.0, 3.0, 4.0], [10.0, np.nan, -5.0, 0.0]])
    ranked = cross_sectional_rank(wide)

    assert ranked.iloc[0].tolist() == pytest.approx([-0.25, 0.0, 0.25, 0.5], abs=1e-12)
    assert ranked.abs().max().max() <= 0.5
    assert ranked.iloc[1].isna().sum() == 1


def test_cross_sectional_zscore_winsorizes_a_delisting():
    """An unclipped z-score on a departure date is a dummy for the departure."""
    row = [0.001] * 50 + [-0.30]
    wide = pd.DataFrame([row])

    unclipped = cross_sectional_zscore(wide, winsorize=0.0)
    assert unclipped.iloc[0, -1] < -6.0

    clipped = cross_sectional_zscore(wide, winsorize=3.0)
    assert clipped.iloc[0, -1] == pytest.approx(-3.0)


def test_features_are_restricted_to_membership_days(panel):
    churning = generate_panel(PanelSpec(n_securities=25, n_periods=150, seed=46,
                                        delist_hazard=0.004))
    built = build_features(churning.frame, [exposure_feature()])
    assert built.index.isin(churning.frame.index).all()


def test_an_unknown_transform_or_column_is_an_error(panel):
    with pytest.raises(ValueError):
        Feature(source="ret", transform="does_not_exist")
    with pytest.raises(ValueError):
        Feature(source="ret", transform="mean", window=0)
    with pytest.raises(KeyError):
        build_features(panel.frame, [Feature(source="not_a_column")])
    with pytest.raises(ValueError):
        build_features(panel.frame, [])
