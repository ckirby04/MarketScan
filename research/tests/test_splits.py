"""M5: the purge and the embargo do what they claim.

`test_no_training_label_overlaps_a_test_label` is the definition-of-done test:
zero overlap between training and test label windows. The rest establish that the
folds are date blocks rather than shuffled rows, that the embargo is applied on
top of the purge, and that the cost of both is reported rather than hidden.
"""

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from research.labels import forward_return
from research.panel import PanelSpec, generate_panel
from research.splits import PurgedKFold


@pytest.fixture(scope="module")
def rows():
    """A panel's worth of (date, label_end) pairs, horizon 10."""
    panel = generate_panel(PanelSpec(n_securities=20, n_periods=300, seed=61))
    labels = forward_return(panel.frame, horizon=10)
    dates = labels.values.index.get_level_values(0)
    return pd.Series(dates), labels.end.reset_index(drop=True), 10


def test_folds_are_contiguous_date_blocks(rows):
    dates, ends, _ = rows
    cv = PurgedKFold(n_splits=5, embargo=0)

    seen: list[pd.Timestamp] = []
    for _, test in cv.split(dates, ends):
        block = dates.iloc[test].unique()
        assert len(block) == len(set(block))
        seen.extend(block)

    # Every date lands in exactly one fold, and the folds are in order.
    assert sorted(seen) == seen
    assert len(set(seen)) == dates.nunique()


def test_a_date_is_never_split_across_folds(rows):
    """The whole cross-section of one date belongs to one fold.

    A date split between train and test leaks that date's market move, which is
    shared by every name in it.
    """
    dates, ends, _ = rows
    cv = PurgedKFold(n_splits=4, embargo=3)
    for train, test in cv.split(dates, ends):
        assert set(dates.iloc[train]).isdisjoint(set(dates.iloc[test]))


@pytest.mark.parametrize("horizon,embargo", [(1, 0), (5, 0), (10, 5), (20, 10)])
def test_no_training_label_overlaps_a_test_label(horizon, embargo):
    """M5's definition of done.

    A training row and a test row must not share a single period of return. The
    check is on the label windows themselves -- (date, label_end] -- not on the
    horizon the caller asked for, because a truncated label spans less than the
    horizon and the assertion has to be about what the labels actually are.
    """
    panel = generate_panel(PanelSpec(n_securities=20, n_periods=400, seed=62,
                                     delist_hazard=0.002))
    labels = forward_return(panel.frame, horizon=horizon)
    dates = pd.Series(labels.values.index.get_level_values(0))
    ends = labels.end.reset_index(drop=True)

    cv = PurgedKFold(n_splits=5, embargo=embargo)
    for train, test, report in cv.split_with_reports(dates, ends):
        if train.size == 0:
            continue
        test_open = dates.iloc[test].min()
        test_close = ends.iloc[test].max()

        train_open = dates.iloc[train]
        train_close = ends.iloc[train]

        # Overlap of (a, b] with (c, d] is (a < d) and (c < b).
        overlapping = (train_open < test_close) & (test_open < train_close)
        assert not overlapping.any(), (
            f"{int(overlapping.sum())} training labels overlap fold {report.fold} "
            f"({test_open.date()}..{test_close.date()})")


def test_the_embargo_extends_the_gap_beyond_the_purge():
    """The embargo must remove rows the purge alone would have kept.

    Purging handles shared labels; the embargo handles shared features. If this
    fails, the embargo parameter is decoration.
    """
    panel = generate_panel(PanelSpec(n_securities=20, n_periods=300, seed=63))
    labels = forward_return(panel.frame, horizon=5)
    dates = pd.Series(labels.values.index.get_level_values(0))
    ends = labels.end.reset_index(drop=True)

    without = list(PurgedKFold(n_splits=5, embargo=0).split_with_reports(dates, ends))
    with_embargo = list(PurgedKFold(n_splits=5, embargo=15).split_with_reports(dates, ends))

    assert sum(r.embargoed for _, _, r in without) == 0
    assert sum(r.embargoed for _, _, r in with_embargo) > 0
    for (train_a, _, _), (train_b, _, _) in zip(without, with_embargo):
        assert train_b.size <= train_a.size
        assert set(train_b).issubset(set(train_a))


def test_purging_costs_are_reported(rows):
    """A pipeline that does not know what purging cost it does not know whether
    it threw away three percent of its data or a third."""
    dates, ends, _ = rows
    reports = [r for _, _, r in PurgedKFold(n_splits=5, embargo=5)
               .split_with_reports(dates, ends)]
    assert len(reports) == 5
    assert all(r.purged > 0 for r in reports[:-1])
    assert sum(r.n_test for r in reports) == len(dates)
    assert all(r.n_train > 0 for r in reports)


def test_train_and_test_never_share_a_row(rows):
    dates, ends, _ = rows
    for train, test in PurgedKFold(n_splits=5, embargo=5).split(dates, ends):
        assert np.intersect1d(train, test).size == 0


def test_an_unshuffled_fold_would_fail_this_suite(rows):
    """The control. Plain k-fold on these rows leaks, and here is the proof.

    Without it, the tests above only establish that this implementation is
    self-consistent, not that the thing it is protecting against is real.
    """
    dates, ends, _ = rows
    n = len(dates)
    rng = np.random.default_rng(0)
    shuffled = rng.permutation(n)
    folds = np.array_split(shuffled, 5)

    leaks = 0
    for fold in folds:
        test_open = dates.iloc[fold].min()
        test_close = ends.iloc[fold].max()
        train = np.setdiff1d(np.arange(n), fold)
        overlapping = (dates.iloc[train] < test_close) & (test_open < ends.iloc[train])
        leaks += int(overlapping.sum())
    assert leaks > 0, "shuffled k-fold did not leak, so this suite proves nothing"


def test_an_impossible_configuration_is_rejected():
    with pytest.raises(ValueError):
        PurgedKFold(n_splits=1)
    with pytest.raises(ValueError):
        PurgedKFold(embargo=-1)

    dates = pd.Series(pd.bdate_range("2020-01-01", periods=3))
    with pytest.raises(ValueError):
        list(PurgedKFold(n_splits=5).split(dates, dates))
