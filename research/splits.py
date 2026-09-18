"""Purged and embargoed k-fold cross-validation for panel data.

Spec 2 M5, and the spec forbids plain k-fold for a reason worth writing down.

WHY SHUFFLED K-FOLD IS WRONG HERE

A row in this panel is (date, security), and its label is a return realized over
the `h` periods *after* that date. Two rows one period apart therefore share
`h-1` periods of return. Shuffle the rows into folds and a training row's label
is built from the same days as a test row's label: the model is fitted on the
answer it is about to be tested on. The score that comes back is not a forecast,
it is a memory, and it is high enough to be convincing.

Worse, the whole cross-section of one date shares that date's market move, so a
single date split across folds leaks the market factor as well.

WHAT THIS DOES INSTEAD

Folds are contiguous blocks of dates -- every security on a date lands in the
same fold -- and a training row is dropped when either of two things is true:

**Purge.** Its label window overlaps the label window of any test row. This is
symmetric and catches both directions: a training row before the block whose
label reaches into it, and a training row just after the block whose label was
still being formed while the block's labels were.

**Embargo.** Its date falls within `embargo` periods after the test block. Purging
handles shared *labels*; the embargo handles shared *features*. The exposure in
these panels has an AR(1) coefficient of 0.95, so a feature measured three days
after a test block still carries most of the information that was in it, and a
model fitted on that row has been shown something very close to the test data
even though no return is shared.

The cost is visible and reported: `FoldReport.purged` counts the rows each fold
threw away. On a panel of 1,000 dates with five folds, a 20-period horizon and a
5-period embargo it is a few percent. A pipeline that does not report it is one
whose author does not know whether it is a few percent or a third.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterator

import numpy as np
import pandas as pd


@dataclass(frozen=True)
class FoldReport:
    """What one fold did, including what it refused to train on."""

    fold: int
    test_start: pd.Timestamp
    test_end: pd.Timestamp
    n_train: int
    n_test: int
    purged: int
    embargoed: int

    def __str__(self) -> str:
        return (f"fold {self.fold}: test {self.test_start.date()}..{self.test_end.date()} "
                f"n_train={self.n_train} n_test={self.n_test} "
                f"purged={self.purged} embargoed={self.embargoed}")


@dataclass(frozen=True)
class PurgedKFold:
    """Contiguous date-block folds with purging and an embargo.

    `embargo` is in periods, not a fraction, because a fraction of a fold is a
    different number of days on every panel and the quantity that matters is how
    long the features stay autocorrelated.
    """

    n_splits: int = 5
    embargo: int = 0

    def __post_init__(self) -> None:
        if self.n_splits < 2:
            raise ValueError("n_splits must be at least 2")
        if self.embargo < 0:
            raise ValueError("embargo cannot be negative")

    def date_blocks(self, dates: pd.DatetimeIndex) -> list[tuple[int, int]]:
        """The folds, as inclusive position ranges into the sorted date index."""
        unique = pd.DatetimeIndex(pd.unique(dates)).sort_values()
        if len(unique) < self.n_splits:
            raise ValueError(f"{len(unique)} dates cannot be split into {self.n_splits} folds")
        bounds = np.linspace(0, len(unique), self.n_splits + 1).astype(int)
        return [(int(bounds[i]), int(bounds[i + 1]) - 1) for i in range(self.n_splits)]

    def split(self, dates: pd.Series | pd.DatetimeIndex,
              label_end: pd.Series | pd.DatetimeIndex) -> Iterator[tuple[np.ndarray, np.ndarray]]:
        """Yields (train positions, test positions) for each fold."""
        for train, test, _ in self.split_with_reports(dates, label_end):
            yield train, test

    def split_with_reports(
        self,
        dates: pd.Series | pd.DatetimeIndex,
        label_end: pd.Series | pd.DatetimeIndex,
    ) -> Iterator[tuple[np.ndarray, np.ndarray, FoldReport]]:
        """As `split`, and says what it dropped.

        `dates` is the date of each row; `label_end` is the date of the last
        return compounded into that row's label. Both are per-row and aligned.
        """
        row_dates = pd.DatetimeIndex(pd.Series(dates).to_numpy())
        row_label_end = pd.DatetimeIndex(pd.Series(label_end).to_numpy())
        if len(row_dates) != len(row_label_end):
            raise ValueError("dates and label_end must be the same length")

        unique = pd.DatetimeIndex(pd.unique(row_dates)).sort_values()
        # Positions rather than timestamps from here on: an embargo is a count of
        # periods, and counting periods on a calendar means counting the
        # panel's own dates, not days.
        position = pd.Series(np.arange(len(unique)), index=unique)
        row_position = position.reindex(row_dates).to_numpy()
        # A label's end may fall on a date the panel has (it always does here),
        # but reindex on a missing date would give NaN, so map through
        # searchsorted, which is also what makes a truncated label -- one that
        # ended early because the name left -- land on its real end.
        label_position = np.searchsorted(unique.to_numpy(), row_label_end.to_numpy(), side="left")

        for fold, (start, end) in enumerate(self.date_blocks(unique)):
            is_test = (row_position >= start) & (row_position <= end)
            test_positions = np.flatnonzero(is_test)
            if test_positions.size == 0:
                continue

            # The span the test fold's labels actually occupy: from the first
            # test date to the last date any test label reaches. Derived from the
            # labels rather than from the horizon, because a truncated label
            # reaches less far and purging on the nominal horizon would throw
            # away training rows for an overlap that does not exist.
            test_label_end = int(label_position[test_positions].max())

            # Inclusive on both ends: a label that closes on the fold's first day
            # and a feature dated on the last day the fold's labels reach are
            # both purged. Off by one in the direction of dropping a row that
            # might have been usable, which costs a little data; the other
            # direction costs the assertion.
            overlaps = (label_position >= start) & (row_position <= test_label_end)
            embargo_end = min(test_label_end + self.embargo, len(unique) - 1)
            embargoed = (row_position > test_label_end) & (row_position <= embargo_end)

            train_mask = ~is_test & ~overlaps & ~embargoed
            train_positions = np.flatnonzero(train_mask)

            report = FoldReport(
                fold=fold,
                test_start=unique[start],
                test_end=unique[end],
                n_train=int(train_positions.size),
                n_test=int(test_positions.size),
                purged=int(np.count_nonzero(~is_test & overlaps)),
                embargoed=int(np.count_nonzero(~is_test & ~overlaps & embargoed)),
            )
            yield train_positions, test_positions, report
