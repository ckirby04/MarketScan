"""Forward-return labels, and the question of what a dead name's label is.

Spec 2 M5. A label is the return realized over the `horizon` periods *after* the
date the features were known, so the pair (features at t, label at t) never
contains a value from before t on one side and after t on the other. That is the
whole of point-in-time safety on the label side; `features.py` handles the other.

THE INTERESTING CASE IS DEATH

A name with four periods left and a twenty-period horizon has no twenty-period
forward return. There are two defensible things to do and they are not equally
honest, so both are implemented and the caller has to choose:

`terminal="compound"` (the default) compounds what there is. If the name left
the universe, its final period carries the departure return, so the label is the
real answer to "what happened to a holder of this name": four periods of return
ending in -30%, and the position is gone. The label is marked truncated.

`terminal="drop"` makes it NaN, which is what a pipeline does when nobody thought
about it. Every name that died is then absent from the labels on the dates
leading up to its death -- which are exactly the dates on which it was heading
there -- and the losses leave the study without anybody removing them. M7's
delisting test measures the gap between these two settings. That number is the
cost of not thinking about it.

A name still alive on the panel's final date is a different matter: its label is
NaN under both settings, because the return has not happened yet. Censoring is
not a loss to be recovered, it is the edge of the data.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

import numpy as np
import pandas as pd

Terminal = Literal["compound", "drop"]


@dataclass(frozen=True)
class Labels:
    """Forward returns, and the metadata purging needs.

    `end` is the date of the last return compounded into each label. The purged
    cross-validation in `splits.py` reads it and nothing else: a fold cannot know
    which training rows overlap it without being told where each label's window
    closes, and inferring it from the horizon would be wrong for exactly the
    truncated rows that matter.
    """

    values: pd.Series
    end: pd.Series
    truncated: pd.Series
    horizon: int
    terminal: Terminal

    def __len__(self) -> int:
        return len(self.values)

    def dropna(self) -> "Labels":
        keep = self.values.dropna().index
        return Labels(self.values.loc[keep], self.end.loc[keep], self.truncated.loc[keep],
                      self.horizon, self.terminal)

    def describe(self) -> str:
        n_truncated = int(self.truncated.sum())
        return (f"labels: horizon={self.horizon}, {int(self.values.notna().sum())} usable, "
                f"{n_truncated} truncated by a departure, terminal={self.terminal}")


def forward_return(frame: pd.DataFrame, horizon: int = 1, terminal: Terminal = "compound",
                   return_column: str = "ret",
                   delisting_returns: dict[str, float] | None = None) -> Labels:
    """The compounded return over the `horizon` periods after each date.

    `frame` is a panel frame: MultiIndex (date, security), holding a
    `return_column` and a boolean `delisted` marking the final period of a name
    that left.

    `delisting_returns` is what happened *after* the last quoted bar, by
    security, and it is empty under the default delisting model -- see
    `research/delisting.py`. Where a name has one, every label whose window
    reaches its departure compounds it as one further factor. That is the
    difference between a study that knows a position went to zero and one that
    believes it was sold at the last print.
    """
    delisting_returns = delisting_returns or {}
    if horizon < 1:
        raise ValueError("horizon must be at least one period")
    if terminal not in ("compound", "drop"):
        raise ValueError(f"unknown terminal policy {terminal!r}")

    returns = frame[return_column].unstack(level=1).sort_index()
    # `eq(True)` rather than `fillna(False)`: unstacking a boolean column over a
    # sparse panel produces object dtype holding NaN, and filling that is a
    # silent downcast pandas warns about. Comparing is exact and stays boolean.
    delisted = (frame["delisted"].unstack(level=1).reindex_like(returns).eq(True)
                if "delisted" in frame.columns
                else pd.DataFrame(False, index=returns.index, columns=returns.columns))

    dates = returns.index
    columns = returns.columns
    n_periods = len(dates)
    values = np.full(returns.shape, np.nan)
    end_position = np.full(returns.shape, -1, dtype=int)
    truncated = np.zeros(returns.shape, dtype=bool)

    observed = returns.notna().to_numpy()
    growth = np.where(observed, 1.0 + returns.to_numpy(), np.nan)
    died = delisted.to_numpy(dtype=bool)

    for column in range(returns.shape[1]):
        present = np.flatnonzero(observed[:, column])
        if present.size == 0:
            continue
        first, last = present[0], present[-1]
        # A name that left carries its departure return on its final row, so its
        # history ends there and the ending is informative. A name still present
        # on the panel's last date is censored: its history ends because the data
        # does, which tells a study nothing and must not be mistaken for an exit.
        name_died = bool(died[last, column])

        for t in range(first, last):
            stop = min(t + horizon, last)
            if stop - t < horizon and not (name_died and stop == last):
                continue  # censored: the horizon runs past the edge of the data
            if stop - t < horizon:
                if terminal == "drop":
                    continue
                truncated[t, column] = True
            window = growth[t + 1:stop + 1, column]
            if not np.all(np.isfinite(window)):
                continue  # a hole inside a membership period; nothing to compound
            growth_total = float(np.prod(window))
            if name_died and stop == last:
                extra = delisting_returns.get(str(columns[column]))
                if extra is not None:
                    growth_total *= (1.0 + extra)
            values[t, column] = growth_total - 1.0
            end_position[t, column] = stop

    index = returns.index
    value_frame = pd.DataFrame(values, index=index, columns=columns)
    truncated_frame = pd.DataFrame(truncated, index=index, columns=columns)

    # A NaT with an explicit unit. `np.datetime64("NaT")` carries the *generic*
    # unit, which numpy 2.5 deprecates and a future version will reject -- and
    # which `pytest`'s error-on-warning setting turns into a failed test suite the
    # first time CI installs a newer numpy than the one on the developer's
    # machine. It did.
    safe_positions = np.clip(end_position, 0, n_periods - 1)
    end_dates = np.where(end_position >= 0, dates.to_numpy()[safe_positions],
                         np.datetime64("NaT", "ns"))
    end_frame = pd.DataFrame(end_dates, index=index, columns=columns)

    def stack(wide: pd.DataFrame) -> pd.Series:
        stacked = wide.stack()
        stacked.index.names = ["date", "security"]
        return stacked.sort_index()

    value_series = stack(value_frame).dropna()
    keep = value_series.index
    return Labels(
        values=value_series,
        end=stack(end_frame).reindex(keep),
        truncated=stack(truncated_frame).reindex(keep).astype(bool),
        horizon=horizon,
        terminal=terminal,
    )
