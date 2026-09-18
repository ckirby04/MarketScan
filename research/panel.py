"""Synthetic market panels with a signal of known strength.

Spec 2 M4. This is the calibration rig for everything in `research/`: a panel of
N securities over T periods whose returns are constructed, not observed, so that
every question the pipeline answers has an answer known in advance.

THE MODEL

    r_{i,t} = beta_i * m_t  +  lam * x_{i,t-1}  +  eps_{i,t}

`m_t` is a market factor common to every name, `beta_i` is a per-name loading on
it, `x_{i,t}` is a per-name exposure generated as an AR(1) process, `eps_{i,t}`
is idiosyncratic noise, and **`lam` is a number you choose**.

THE LAG IS THE POINT

The spec writes that equation with a single time index. It is written here with
the exposure lagged one period, and the difference is the whole distinction
between a pipeline that is honest and one that is not: `x_{i,t}` is the value an
observer knows at the close of period `t`, and the return it explains is the one
realized over period `t+1`. An exposure contemporaneous with the return it
predicts is a lookahead feature, and M7 injects exactly that on purpose to watch
the guards fire. Here it is lagged.

So the relationship the pipeline is asked to find is between `exposure` on date
`t` and the *forward* return, and `lam` is in units of return per unit of
exposure per period: `lam = 0.0005` is five basis points a day for a one
standard deviation exposure, because `x` is generated with unit unconditional
variance.

CHURN, AND WHY IT IS HERE

With `delist_hazard` above zero, names leave the panel and new ones arrive. Two
milestones need this. The universe machinery in the C++ engine needs a
membership history that actually changes to be exercised at all (M9), and M7's
survivorship test needs the thing survivorship bias hides: a name that left, and
the return it left on. A departing name's final period carries
`delist_return` -- by default -30%, and -100% in the delisting pathology -- and
a panel rebuilt from the survivors of the final period has never seen it.

`delist_exposure_tilt` makes the hazard depend on the exposure, so that the
names that die are disproportionately the ones the signal disliked. That is what
makes survivorship bias *inflate* a measured premium rather than merely add
noise, and M7 measures by how much.

DETERMINISM

One seed. Each random quantity draws from its own stream spawned off that seed,
for the same reason each theory in the C++ engine gets its own RNG stream:
turning up the noise scale should not silently redraw the betas, and a change to
the churn model should not change the exposures of a panel generated to test
something else.
"""

from __future__ import annotations

import dataclasses
from dataclasses import dataclass

import numpy as np
import pandas as pd

# Stream names, spawned in this order off the master seed. Appending to the end
# is safe; reordering changes every panel this module has ever generated.
_STREAMS = ("market", "beta", "sector", "exposure", "noise", "churn", "price")

_ANNUAL_PERIODS = 252


@dataclass(frozen=True)
class PanelSpec:
    """Everything about a panel. Frozen, and carried on the panel it produced.

    A generated panel that does not remember what generated it is a number
    without an experiment attached.
    """

    n_securities: int = 200
    """Names that are members at any one time."""

    n_periods: int = 1000
    """Periods, one business day each."""

    lam: float = 0.0005
    """Signal strength: expected forward return per unit of exposure.

    Zero is a legitimate and important value -- it is the entire content of M8.
    """

    seed: int = 20260918

    beta_mean: float = 1.0
    beta_sd: float = 0.25

    market_vol: float = 0.010
    """Per-period standard deviation of the market factor."""

    idio_vol: float = 0.015
    """Per-period standard deviation of idiosyncratic noise.

    Deliberately larger than the signal by a wide margin. A panel in which the
    signal is easy to see is not a test of anything: the ratio here is roughly
    what a real daily cross-section looks like, which is why recovering `lam`
    needs the whole panel rather than a scatter plot.
    """

    exposure_rho: float = 0.95
    """AR(1) persistence of the exposure.

    Persistent, because an exposure that is fresh noise every period makes
    turnover meaningless and the purged cross-validation in M5 pointless: there
    would be nothing for a label window to leak.
    """

    n_sectors: int = 6
    """Groups for the sector-neutrality constraint. Labelled, not modelled: a
    sector here carries no shared return component, because a sector factor this
    phase did not plant is a signal it cannot check."""

    start_date: str = "2000-01-03"

    price_low: float = 20.0
    price_high: float = 200.0
    """Range of initial prices, drawn log-uniformly. Prices exist for the
    engine handoff and for nothing else -- the research pipeline reads returns.
    """

    delist_hazard: float = 0.0
    """Per-period probability that a member leaves. Zero by default: the
    recovery milestone wants a panel with one thing going on in it."""

    delist_exposure_tilt: float = 0.0
    """How much a low exposure raises the hazard. See the module docstring."""

    delist_return: float = -0.30
    """The return on a departing name's final period.

    -30% is a made-up number of the right order for a real delisting, and the
    only claim made for it is that it is not zero. A backtest that liquidates at
    the last print assumes zero, which is how naive backtests hide their losses.
    """

    volume_mean: float = 1_000_000.0

    def __post_init__(self) -> None:
        if self.n_securities < 2:
            raise ValueError("n_securities must be at least 2; a cross-section of one is not one")
        if self.n_periods < 2:
            raise ValueError("n_periods must be at least 2")
        if not 0.0 <= self.exposure_rho < 1.0:
            raise ValueError("exposure_rho must be in [0, 1); at 1 the exposure is a random walk "
                             "and has no unconditional variance for lam to be measured against")
        if not 0.0 <= self.delist_hazard <= 1.0:
            raise ValueError("delist_hazard is a probability")
        if self.n_sectors < 1:
            raise ValueError("n_sectors must be at least 1")

    def replace(self, **changes: object) -> "PanelSpec":
        """A copy with fields changed. The sweep in M8 lives on this."""
        return dataclasses.replace(self, **changes)  # type: ignore[arg-type]

    def to_dict(self) -> dict[str, object]:
        return dataclasses.asdict(self)


@dataclass
class Panel:
    """A generated panel and the truth about it.

    `frame` is point-in-time by construction: it holds a row for (date,
    security) only on dates when that security was a member. There is no
    survivorship filter to remember to apply, because the rows that would
    constitute survivorship bias are not in it. M7 has to reintroduce them
    deliberately to measure what they are worth.
    """

    spec: PanelSpec
    frame: pd.DataFrame
    """MultiIndex (date, security). Columns: exposure, ret, price, volume,
    beta, sector, delisted."""

    market: pd.Series
    """date -> market factor return."""

    betas: pd.Series
    sectors: pd.Series
    membership: pd.DataFrame
    """One row per membership period: security, start, end (both inclusive
    dates, `end` being the last date the name was a member)."""

    @property
    def dates(self) -> pd.DatetimeIndex:
        return pd.DatetimeIndex(self.frame.index.get_level_values(0).unique()).sort_values()

    @property
    def securities(self) -> pd.Index:
        return self.frame.index.get_level_values(1).unique().sort_values()

    @property
    def survivors(self) -> pd.Index:
        """Names that were still members on the final date.

        The set a survivorship-biased study would build its universe from.
        """
        last = self.dates[-1]
        return self.frame.loc[last].index.sort_values()

    def wide(self, column: str) -> pd.DataFrame:
        """A date x security matrix for one column, NaN where not a member."""
        return self.frame[column].unstack(level=1).sort_index()

    def exposures(self) -> pd.DataFrame:
        return self.wide("exposure")

    def returns(self) -> pd.DataFrame:
        return self.wide("ret")

    def annualization(self) -> float:
        return float(_ANNUAL_PERIODS)

    def describe(self) -> str:
        n_delist = int(self.frame["delisted"].sum())
        return (f"panel: {len(self.securities)} securities over {len(self.dates)} periods "
                f"({self.dates[0].date()} to {self.dates[-1].date()}), "
                f"lam={self.spec.lam:g}, {len(self.membership)} membership periods, "
                f"{n_delist} departures, {len(self.survivors)} survivors")


def _streams(seed: int) -> dict[str, np.random.Generator]:
    children = np.random.SeedSequence(seed).spawn(len(_STREAMS))
    return {name: np.random.default_rng(child) for name, child in zip(_STREAMS, children)}


def _ar1(rng: np.random.Generator, n_periods: int, n_names: int, rho: float) -> np.ndarray:
    """AR(1) columns with unit unconditional variance.

    The first row is drawn from the stationary distribution rather than started
    at zero, so the panel does not open with an exposure that is artificially
    small and a lam that appears to fade in.
    """
    innovation_sd = np.sqrt(1.0 - rho * rho)
    shocks = rng.standard_normal((n_periods, n_names))
    out = np.empty((n_periods, n_names), dtype=float)
    out[0] = shocks[0]
    for t in range(1, n_periods):
        out[t] = rho * out[t - 1] + innovation_sd * shocks[t]
    return out


def generate_panel(spec: PanelSpec | None = None, **overrides: object) -> Panel:
    """Generates a panel from a spec. Deterministic given `spec.seed`."""
    spec = spec or PanelSpec()
    if overrides:
        spec = spec.replace(**overrides)
    rng = _streams(spec.seed)

    dates = pd.bdate_range(spec.start_date, periods=spec.n_periods)
    n_periods = spec.n_periods

    # --- The pool ------------------------------------------------------------
    #
    # Churn needs replacements, and a replacement has to be a name that has not
    # been used, or a ticker would carry two companies' histories -- the exact
    # confusion docs/IDENTITY.md exists to prevent. So the pool is sized for the
    # expected number of departures with generous headroom, and generated up
    # front: drawing a new name's beta at the moment it enters would make every
    # beta in the panel depend on the churn path.
    expected_exits = spec.delist_hazard * spec.n_securities * n_periods
    pool = spec.n_securities + int(np.ceil(2.5 * expected_exits)) + 16
    names = np.array([f"SY{i:04d}" for i in range(pool)])

    betas = rng["beta"].normal(spec.beta_mean, spec.beta_sd, size=pool)
    sectors = rng["sector"].integers(0, spec.n_sectors, size=pool)
    initial_prices = np.exp(rng["price"].uniform(np.log(spec.price_low),
                                                 np.log(spec.price_high), size=pool))

    market = rng["market"].normal(0.0, spec.market_vol, size=n_periods)

    # One extra row of exposure: the exposure observed on the date *before* the
    # first return is what drives that return, so the matrix is indexed -1..T-1
    # and row 0 is the pre-sample observation.
    exposure = _ar1(rng["exposure"], n_periods + 1, pool, spec.exposure_rho)
    noise = rng["noise"].normal(0.0, spec.idio_vol, size=(n_periods, pool))

    # --- Membership ----------------------------------------------------------
    active = np.zeros(pool, dtype=bool)
    active[: spec.n_securities] = True
    start_index = np.full(pool, -1, dtype=int)
    end_index = np.full(pool, -1, dtype=int)
    start_index[: spec.n_securities] = 0
    delisted_at = np.full(pool, -1, dtype=int)
    next_unused = spec.n_securities
    exhausted = False

    member_mask = np.zeros((n_periods, pool), dtype=bool)
    churn = rng["churn"]

    for t in range(n_periods):
        member_mask[t] = active
        if spec.delist_hazard <= 0.0:
            continue

        # Hazard tilted by the exposure known at the start of the period. A
        # low exposure raises it; the floor at zero keeps a large tilt from
        # producing a negative probability rather than clipping quietly.
        tilt = 1.0 - spec.delist_exposure_tilt * exposure[t]
        hazard = np.clip(spec.delist_hazard * np.maximum(tilt, 0.0), 0.0, 1.0)
        draws = churn.random(pool)
        # Never on the final period: a name whose last period is the panel's
        # last period is indistinguishable from one that simply ran out of data,
        # and calling that a delisting would plant a loss the study could not
        # have known about.
        dying = active & (draws < hazard) if t < n_periods - 1 else np.zeros(pool, dtype=bool)

        for i in np.flatnonzero(dying):
            active[i] = False
            end_index[i] = t
            delisted_at[i] = t
            if next_unused < pool:
                # The replacement joins on the following period.
                active[next_unused] = True
                start_index[next_unused] = t + 1
                next_unused += 1
            else:
                exhausted = True

    if exhausted:
        raise RuntimeError(
            "the replacement pool ran out: the realized churn exceeded what the hazard "
            "predicted by more than the headroom allows. Raise the headroom in generate_panel "
            "rather than letting the panel shrink, which would be survivorship bias "
            "introduced by the generator itself")

    for i in range(pool):
        if start_index[i] >= 0 and end_index[i] < 0:
            end_index[i] = n_periods - 1

    # --- Returns -------------------------------------------------------------
    #
    # exposure[t] is the value observed at the end of period t-1 in return
    # terms: row t of `exposure` drives row t of `ret`.
    ret = betas[None, :] * market[:, None] + spec.lam * exposure[:n_periods, :] + noise

    delisted = np.zeros((n_periods, pool), dtype=bool)
    for i in np.flatnonzero(delisted_at >= 0):
        t = delisted_at[i]
        ret[t, i] = spec.delist_return
        delisted[t, i] = True

    # Prices compound the returns that the panel actually shows, so a bar file
    # exported for the engine and a return column read by the pipeline cannot
    # disagree about what happened.
    price = np.full((n_periods, pool), np.nan)
    for i in range(pool):
        if start_index[i] < 0:
            continue
        lo, hi = start_index[i], end_index[i]
        compounded = np.cumprod(1.0 + ret[lo:hi + 1, i])
        price[lo:hi + 1, i] = initial_prices[i] * compounded

    volume = rng["price"].lognormal(np.log(spec.volume_mean), 0.4, size=(n_periods, pool))

    # --- Assembly ------------------------------------------------------------
    #
    # The observable exposure on date t is exposure[t + 1]: the one that drives
    # the return of period t+1, which is what a study on date t is trying to
    # predict. Labelled `exposure` because that is what the pipeline consumes;
    # the shift is done here, once, rather than left for every caller to get
    # wrong.
    observable = exposure[1:n_periods + 1, :]

    rows = np.flatnonzero(member_mask.ravel())
    date_index = np.repeat(np.arange(n_periods), pool)[rows]
    name_index = np.tile(np.arange(pool), n_periods)[rows]

    frame = pd.DataFrame(
        {
            "exposure": observable.ravel()[rows],
            "ret": ret.ravel()[rows],
            "price": price.ravel()[rows],
            "volume": volume.ravel()[rows],
            "beta": betas[name_index],
            "sector": sectors[name_index],
            "delisted": delisted.ravel()[rows],
        },
        index=pd.MultiIndex.from_arrays([dates[date_index], names[name_index]],
                                        names=["date", "security"]),
    ).sort_index()

    used = np.flatnonzero(start_index >= 0)
    membership = pd.DataFrame({
        "security": names[used],
        "start": dates[start_index[used]],
        "end": dates[end_index[used]],
    }).sort_values(["start", "security"]).reset_index(drop=True)

    return Panel(
        spec=spec,
        frame=frame,
        market=pd.Series(market, index=dates, name="market"),
        betas=pd.Series(betas[used], index=names[used], name="beta"),
        sectors=pd.Series(sectors[used], index=names[used], name="sector"),
        membership=membership,
    )
