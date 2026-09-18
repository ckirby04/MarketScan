"""Generic panel transforms, and the reason none of them has a financial name.

Spec 2 M5. A feature here is a causal transform of one panel column: every value
a feature takes on date `t` is a function of observations on or before `t`, for
that security, and of the cross-section on `t` alone. There is no configuration
in which a transform in this module can see the future, and `research/tests/
test_features.py` asserts that by perturbing tomorrow and checking that today
does not move.

NO NAMED FACTORS

The spec forbids momentum, value, quality or any other real factor in this phase,
and the prohibition is not cosmetic. The only signal any of these panels contains
is the synthetic exposure `x`, whose strength is known; a transform called
`momentum` would invite the reading that recovering it said something about
markets. So what exists here is `mean(source, window)` -- an average of a column
over a window. Point it at a return column and you have built a well-known factor
by accident, and the honest description of what you then have is "an average of
past returns on synthetic data", which is exactly what the sweep in M8 wants: a
large number of plausible-looking transforms of data containing no signal.

THE ONE EXCEPTION, DELIBERATELY NAMED

`leak` shifts a column backwards in time. It is the lookahead pathology M7
injects, it is spelled out in the registry beside the honest transforms so it
cannot be reached by a typo, and every result computed with it is labelled. A
codebase with no way to express the mistake cannot measure what the mistake is
worth.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Literal

import numpy as np
import pandas as pd

Wide = pd.DataFrame
"""A date x security matrix. NaN where the security was not a member."""

CrossSection = Literal["none", "rank", "zscore"]


# ---------------------------------------------------------------------------
# Time-series transforms. Each takes a wide matrix and returns one.
# ---------------------------------------------------------------------------


def _level(wide: Wide, window: int) -> Wide:
    return wide


def _lag(wide: Wide, window: int) -> Wide:
    return wide.shift(max(window, 1))


def _mean(wide: Wide, window: int) -> Wide:
    return wide.rolling(window, min_periods=window).mean()


def _sd(wide: Wide, window: int) -> Wide:
    return wide.rolling(window, min_periods=window).std(ddof=1)


def _zscore_ts(wide: Wide, window: int) -> Wide:
    rolling = wide.rolling(window, min_periods=window)
    sd = rolling.std(ddof=1)
    return (wide - rolling.mean()) / sd.where(sd > 0)


def _change(wide: Wide, window: int) -> Wide:
    return wide - wide.shift(window)


def _ewm(wide: Wide, window: int) -> Wide:
    # `min_periods` matters as much here as in the rolling transforms: an EWM
    # seeded from a single observation is that observation, and a feature whose
    # first value is raw noise pretends to a history it does not have.
    return wide.ewm(halflife=max(window, 1), min_periods=window, adjust=True).mean()


def _compound(wide: Wide, window: int) -> Wide:
    """Compounded growth over the window. For a column of returns."""
    return (1.0 + wide).rolling(window, min_periods=window).apply(np.prod, raw=True) - 1.0


def _rank_ts(wide: Wide, window: int) -> Wide:
    """Where the current value sits within its own trailing window, on [0, 1]."""
    return wide.rolling(window, min_periods=window).rank(pct=True)


def _sign(wide: Wide, window: int) -> Wide:
    return np.sign(wide)


def _leak(wide: Wide, window: int) -> Wide:
    """The pathology. Shifts the future into the present. See M7.

    Every use of this is a deliberate one, and `Feature.leaks` reports it so a
    result computed with it cannot be presented as though it were not.
    """
    return wide.shift(-max(window, 1))


TRANSFORMS: dict[str, Callable[[Wide, int], Wide]] = {
    "level": _level,
    "lag": _lag,
    "mean": _mean,
    "sd": _sd,
    "zscore": _zscore_ts,
    "change": _change,
    "ewm": _ewm,
    "compound": _compound,
    "rank": _rank_ts,
    "sign": _sign,
    "leak": _leak,
}

_CAUSAL = {name for name in TRANSFORMS if name != "leak"}


# ---------------------------------------------------------------------------
# Cross-sectional normalization, per date
# ---------------------------------------------------------------------------


def cross_sectional_rank(wide: Wide) -> Wide:
    """Ranks each date's cross-section onto [-0.5, +0.5].

    Centred so that the result is a zero-sum tilt before the portfolio layer
    sees it, and scale-free so that a date with an unusually wide cross-section
    does not get an unusually large position. Ties share a rank.
    """
    ranked = wide.rank(axis=1, pct=True, na_option="keep")
    return ranked - 0.5


def cross_sectional_zscore(wide: Wide, winsorize: float = 3.0) -> Wide:
    """Standardizes each date's cross-section, clipping at `winsorize` sigma.

    The clip is not decoration. A departing name's final period is -30% or
    -100%, and an unwinsorized z-score of a return column on that date is one
    name at -12 sigma and everything else at zero -- a feature that is, for that
    date, a dummy variable for the delisting.
    """
    mean = wide.mean(axis=1)
    sd = wide.std(axis=1, ddof=1)
    z = wide.sub(mean, axis=0).div(sd.where(sd > 0), axis=0)
    return z.clip(-winsorize, winsorize) if winsorize else z


def _normalize(wide: Wide, how: CrossSection) -> Wide:
    if how == "none":
        return wide
    if how == "rank":
        return cross_sectional_rank(wide)
    if how == "zscore":
        return cross_sectional_zscore(wide)
    raise ValueError(f"unknown cross-sectional normalization {how!r}")


# ---------------------------------------------------------------------------
# Features
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Feature:
    """One transform of one panel column, with a name to report it under."""

    source: str
    transform: str = "level"
    window: int = 1
    cross_section: CrossSection = "zscore"
    name: str | None = None

    def __post_init__(self) -> None:
        if self.transform not in TRANSFORMS:
            raise ValueError(f"unknown transform {self.transform!r}; "
                             f"have {sorted(TRANSFORMS)}")
        if self.window < 1:
            raise ValueError("window must be at least one period")

    @property
    def label(self) -> str:
        return self.name or f"{self.source}.{self.transform}{self.window}.{self.cross_section}"

    @property
    def leaks(self) -> bool:
        """Whether this feature can see the future. True only for `leak`."""
        return self.transform not in _CAUSAL

    def build(self, wide: Wide) -> Wide:
        return _normalize(TRANSFORMS[self.transform](wide, self.window), self.cross_section)


def exposure_feature(cross_section: CrossSection = "zscore") -> Feature:
    """The exposure itself, as the pipeline's honest baseline feature.

    The one feature that is *supposed* to work: the panel planted a relationship
    between this and the forward return, and a pipeline that cannot find it here
    is broken rather than sceptical.
    """
    return Feature(source="exposure", transform="level", window=1,
                   cross_section=cross_section, name="exposure")


def build_features(frame: pd.DataFrame, features: list[Feature]) -> pd.DataFrame:
    """Builds every feature, returning a (date, security) x feature frame.

    Restricted at the end to the rows the panel actually has, so a rolling window
    cannot emit a value for a date on which the security was not a member -- the
    same structural guard the C++ engine applies to dispatch, applied here to
    features.
    """
    if not features:
        raise ValueError("a pipeline with no features is not a pipeline")

    wides: dict[str, Wide] = {}
    columns: dict[str, pd.Series] = {}
    for feature in features:
        if feature.source not in frame.columns:
            raise KeyError(f"panel has no column {feature.source!r}")
        if feature.source not in wides:
            wides[feature.source] = frame[feature.source].unstack(level=1).sort_index()
        built = feature.build(wides[feature.source])
        stacked = built.stack()
        stacked.index.names = ["date", "security"]
        columns[feature.label] = stacked

    built_frame = pd.DataFrame(columns).sort_index()
    return built_frame.reindex(frame.index).dropna(how="all")


def leaking(features: list[Feature]) -> list[str]:
    """The features in this set that can see the future. Empty is the norm."""
    return [feature.label for feature in features if feature.leaks]
