"""Active weights, and the constraints that make them a portfolio.

Spec 2 M5. A score is not a portfolio. Turning one into the other is where most
of the distance between a research result and a tradeable one lives, and every
constraint in here removes some of the return the unconstrained version would
have reported.

ACTIVE, NOT ABSOLUTE

The weights this produces are deviations from a benchmark, and they sum to zero
by construction. That is what makes the result comparable to the C++ engine's
benchmark-relative reporting, and it is also why the portfolio's return is
computed as `sum(w_active * r)` with no benchmark term: the benchmark cancels.

The benchmark itself is equal-weight across the members of the day. Equal weight
rather than capitalization weight because these panels have no capitalization --
and inventing one would be inventing a size factor this phase is not allowed to
have.

THE CONSTRAINTS, AND WHAT EACH ONE IS FOR

| sector neutrality | The weights sum to zero within every sector, so the result cannot be a bet on one group of names that the score happened to like. |
| per-name cap | No single name can carry the result. Without it, a score's most extreme value gets the whole book. |
| beta target | The active book's exposure to the market factor is pinned, by default at zero, so market direction is not smuggled in as alpha. |
| turnover budget | A limit on how much the book may change per rebalance. Without one, a persistent-looking signal can be a high-frequency one whose costs eat it. |

HOW THEY ARE ENFORCED

Every constraint above is a convex set, so the book is found by projecting onto
each in turn until the sequence stops moving: demean within sector, neutralize
beta along the sector-demeaned beta vector (which cannot disturb the sector sums),
scale the gross down if it exceeds the budget, clip to the per-name cap, and
project the trade onto the turnover ball around yesterday's book. Each step breaks
the previous one a little, and the cycle converges because they all have to hold
at once and a point satisfying all of them exists.

The turnover budget is projected onto *inside* that loop, not applied to the
finished book. The first implementation blended the compliant target with
yesterday's book afterwards, and it was wrong in a way worth recording: overnight
the universe removes names, so yesterday's book is *not* compliant -- it no longer
sums to zero -- and a blend between a compliant point and a non-compliant one is
not compliant either. The books it produced were a few basis points off neutral
while the code reported neutrality.

The gross is therefore an inequality rather than a target. A book held back by its
turnover budget carries less exposure than it wanted to, and inflating it back to
target would be trading the budget does not allow.

The builder **checks its own output** and raises `ConstraintError` if anything is
violated. A portfolio builder that quietly returns a book breaking the limits it
advertises is worse than one that fails, because the number it reports looks
exactly like the number a constrained book would have earned.

NO COSTS, DELIBERATELY

There are no commissions, no spread and no slippage in here, and the turnover
budget is the only thing standing in for them. That is not an oversight: the C++
engine owns execution, it crosses the spread by default, and M9 puts the same
strategy through both to compare. A second, approximate cost model in the research
layer would be a second answer to a question that already has one -- and the
disagreement it produced at M9 would be between two cost models rather than
between the research and the engine.

Which means every Sharpe this module produces is a frictionless Sharpe, and reads
far better than anything tradeable. `research/README.md` says so where somebody
reading a number will see it.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd


class ConstraintError(RuntimeError):
    """Raised when a built portfolio violates a constraint it claims to satisfy."""


@dataclass(frozen=True)
class PortfolioConstraints:
    gross: float = 1.0
    """Maximum sum of absolute active weights. One unit of book per unit of
    benchmark: a gross of 1.0 against a 1.0 benchmark is a 50/50 long/short
    overlay."""

    max_weight: float = 0.02
    """Per-name cap on the absolute active weight."""

    sector_neutral: bool = True
    beta_target: float = 0.0
    turnover_budget: float = 0.20
    """Maximum sum of absolute weight change per rebalance. Inf for none."""

    tolerance: float = 1e-6
    max_iterations: int = 200

    def __post_init__(self) -> None:
        if self.gross <= 0.0:
            raise ValueError("gross must be positive")
        if self.max_weight <= 0.0:
            raise ValueError("max_weight must be positive")
        if self.turnover_budget <= 0.0:
            raise ValueError("turnover_budget must be positive")

    def feasible_for(self, n_names: int) -> bool:
        """Whether a cap this tight can reach this gross at all.

        n * cap < gross means every name is pinned at the cap and the gross still
        cannot be met. Worth checking rather than discovering as a failed
        assertion inside the projection loop.
        """
        return n_names * self.max_weight >= self.gross - self.tolerance


def violations(weights: pd.Series, betas: pd.Series, sectors: pd.Series,
               constraints: PortfolioConstraints,
               previous: pd.Series | None = None) -> list[str]:
    """Every constraint `weights` breaks, named. Empty means compliant.

    Used by the builder on its own output and by the tests to confirm that a
    deliberately broken book is rejected.
    """
    found: list[str] = []
    tol = max(constraints.tolerance, 1e-9) * 10.0

    if weights.empty:
        return ["empty portfolio"]
    if not np.isfinite(weights.to_numpy()).all():
        found.append("non-finite weights")
        return found

    total = float(weights.sum())
    if abs(total) > tol:
        found.append(f"weights do not sum to zero: {total:+.2e}")

    gross = float(weights.abs().sum())
    if gross > constraints.gross + tol:
        found.append(f"gross {gross:.4f} exceeds {constraints.gross:.4f}")

    worst = float(weights.abs().max())
    if worst > constraints.max_weight + tol:
        found.append(f"max weight {worst:.4f} exceeds cap {constraints.max_weight:.4f}")

    if constraints.sector_neutral:
        by_sector = weights.groupby(sectors.reindex(weights.index)).sum()
        offending = by_sector[by_sector.abs() > tol]
        if not offending.empty:
            worst_sector = offending.abs().idxmax()
            found.append(f"sector {worst_sector} nets {offending[worst_sector]:+.2e}")

    active_beta = float((weights * betas.reindex(weights.index)).sum())
    if abs(active_beta - constraints.beta_target) > tol:
        found.append(f"active beta {active_beta:+.4f} misses target "
                     f"{constraints.beta_target:+.4f}")

    if previous is not None and np.isfinite(constraints.turnover_budget):
        combined = weights.reindex(weights.index.union(previous.index)).fillna(0.0)
        before = previous.reindex(combined.index).fillna(0.0)
        turnover = float((combined - before).abs().sum())
        if turnover > constraints.turnover_budget + tol:
            found.append(f"turnover {turnover:.4f} exceeds budget "
                         f"{constraints.turnover_budget:.4f}")

    return found


def _project_l1_ball(step: np.ndarray, radius: float) -> np.ndarray:
    """Euclidean projection of `step` onto {x : sum|x| <= radius}.

    Soft-thresholding at the level that makes the norm exactly the radius. The
    textbook algorithm, here because the turnover budget is a constraint like any
    other and the alternative -- scaling the whole trade down uniformly -- is not
    a projection: it shrinks the trades that matter by the same factor as the
    ones that do not.
    """
    if radius <= 0.0:
        return np.zeros_like(step)
    magnitude = np.abs(step)
    if float(magnitude.sum()) <= radius:
        return step
    descending = np.sort(magnitude)[::-1]
    cumulative = np.cumsum(descending)
    counts = np.arange(1, descending.size + 1)
    feasible = descending - (cumulative - radius) / counts > 0
    last = int(np.flatnonzero(feasible)[-1])
    threshold = (cumulative[last] - radius) / (last + 1)
    return np.sign(step) * np.maximum(magnitude - threshold, 0.0)


def _project(scores: np.ndarray, betas: np.ndarray, sector_codes: np.ndarray,
             constraints: PortfolioConstraints,
             previous: np.ndarray | None = None) -> np.ndarray:
    """Alternating projection onto the constraint set. See the module docstring.

    `previous` is yesterday's weight for each of today's names, and the turnover
    ball around it is projected onto inside the loop rather than applied to the
    finished book. Applying it afterwards was the first implementation and it was
    wrong: a blend between a compliant target and a previous book that is *not*
    compliant -- because the universe removed names from it overnight -- is not
    compliant either, and the result was a book whose sector sums were a few
    basis points off while the code reported neutrality.
    """
    weights = scores - scores.mean()

    # Sector-demeaned betas: adding a multiple of this vector cannot disturb the
    # sector sums, and its inner product with the raw betas equals its own
    # squared norm, so the beta correction is exact in one step rather than
    # another thing to iterate.
    sector_index = {code: np.flatnonzero(sector_codes == code)
                    for code in np.unique(sector_codes)}
    if constraints.sector_neutral:
        beta_free = betas.copy()
        for rows in sector_index.values():
            beta_free[rows] -= betas[rows].mean()
    else:
        beta_free = betas - betas.mean()
    beta_norm = float(np.dot(beta_free, beta_free))

    # Open at the target gross. Afterwards the gross is only ever reduced, which
    # is what makes it an inequality the turnover ball can respect: a book held
    # back from its target carries less exposure, and inflating it back would be
    # trading that the budget does not allow.
    scale = float(np.abs(weights).sum())
    if scale > 0.0:
        weights *= constraints.gross / scale

    budget = constraints.turnover_budget if previous is not None else float("inf")

    for _ in range(constraints.max_iterations):
        before = weights.copy()

        if constraints.sector_neutral:
            for rows in sector_index.values():
                weights[rows] -= weights[rows].mean()
        else:
            weights -= weights.mean()

        if beta_norm > 0.0:
            drift = float(np.dot(weights, betas)) - constraints.beta_target
            weights -= (drift / beta_norm) * beta_free

        # Scaled to the target rather than merely capped at it. Combined with the
        # clip on the next line this water-fills: names at the cap stay there,
        # the rest grow, and the gross converges up to target instead of settling
        # wherever the first clip left it -- which was five percent short of
        # target whenever the cap bound, and is a book quietly smaller than the
        # one whose returns are being reported.
        gross = float(np.abs(weights).sum())
        if gross > 0.0:
            weights *= constraints.gross / gross

        np.clip(weights, -constraints.max_weight, constraints.max_weight, out=weights)

        if previous is not None and np.isfinite(budget):
            weights = previous + _project_l1_ball(weights - previous, budget)

        if float(np.abs(weights - before).sum()) < constraints.tolerance:
            break

    return weights


def build_weights(scores: pd.Series, betas: pd.Series, sectors: pd.Series,
                  constraints: PortfolioConstraints,
                  previous: pd.Series | None = None) -> pd.Series:
    """One date's active weights. Raises if the result breaks a constraint.

    `previous` is yesterday's book restricted to today's names: the positions this
    one is allowed to trade away from, with anything the universe removed already
    gone. Liquidating a departed name is not charged against the turnover budget,
    and that is a decision rather than an oversight -- see `build_portfolio`.
    """
    usable = scores.dropna()
    if usable.empty:
        return pd.Series(dtype=float)
    if not constraints.feasible_for(len(usable)):
        raise ConstraintError(
            f"{len(usable)} names at a cap of {constraints.max_weight:g} cannot reach a gross "
            f"of {constraints.gross:g}; the constraints are infeasible, not tight")

    aligned_previous = (previous.reindex(usable.index).fillna(0.0).to_numpy(dtype=float)
                       if previous is not None else None)

    weights = pd.Series(
        _project(usable.to_numpy(dtype=float),
                 betas.reindex(usable.index).to_numpy(dtype=float),
                 sectors.reindex(usable.index).to_numpy(),
                 constraints,
                 previous=aligned_previous),
        index=usable.index,
    )

    broken = violations(weights, betas, sectors, constraints,
                        previous.reindex(usable.index).fillna(0.0) if previous is not None
                        else None)
    if broken:
        raise ConstraintError(
            "built portfolio violates its own constraints: " + "; ".join(broken) +
            ". Either the turnover budget cannot reach a compliant book from here, or the "
            "projection did not converge in max_iterations")
    return weights


@dataclass
class PortfolioResult:
    """A held book over time, and what it earned."""

    weights: pd.DataFrame
    """date x security active weights, as decided on that date."""

    active_returns: pd.Series
    """Realized on the following date: weights are decided at the close of t and
    earn the return of t+1. Indexed by the date the return was earned, so a
    Sharpe computed from it is a Sharpe of realized returns."""

    turnover: pd.Series
    gross: pd.Series
    rebalances: int
    forced_exits: int
    """Positions closed because the name left the universe."""

    unrepaired: int
    """Dates on which a departure could not be repaired because too few names were
    scored, leaving the book off its constraints until the next rebalance.
    Reported rather than asserted, so that a run with a gap in its scores says so
    instead of failing."""

    thin: int
    """Dates on which a rebuild was due but the scored cross-section was too small
    to make a compliant book. Normal near the end of a long-horizon run, where
    only the names that ended early still have labels."""

    delisting_loss: float
    """Return earned on positions in their departure period. Negative when the
    book was long something that left. Reported separately because this is the
    number a pipeline that drops dead names never sees."""

    def summary(self) -> dict[str, float]:
        from research import metrics
        stats = metrics.summarize(self.active_returns)
        stats["turnover"] = float(self.turnover.mean()) if len(self.turnover) else float("nan")
        stats["gross"] = float(self.gross.mean()) if len(self.gross) else float("nan")
        stats["forced_exits"] = float(self.forced_exits)
        stats["unrepaired"] = float(self.unrepaired)
        stats["thin"] = float(self.thin)
        stats["delisting_loss"] = self.delisting_loss
        return stats


def build_portfolio(scores: pd.Series, panel_frame: pd.DataFrame, betas: pd.Series,
                    sectors: pd.Series, constraints: PortfolioConstraints | None = None,
                    rebalance_every: int = 21) -> PortfolioResult:
    """Runs the book forward over the panel.

    `scores` is MultiIndexed (date, security) -- typically out-of-sample model
    predictions. The book is rebuilt every `rebalance_every` dates and held in
    between, except that a name leaving the universe is always closed on the date
    it leaves.

    Weights are held fixed between rebalances rather than allowed to drift with
    prices. Drift is a real effect and the engine models it; modelling it
    differently here would guarantee a disagreement at M9 that had nothing to do
    with the research code.
    """
    constraints = constraints or PortfolioConstraints()
    if rebalance_every < 1:
        raise ValueError("rebalance_every must be at least one period")

    returns = panel_frame["ret"].unstack(level=1).sort_index()
    # `eq(True)`, not `fillna(False)`: see the note in labels.py.
    delisted = panel_frame["delisted"].unstack(level=1).reindex_like(returns).eq(True)
    score_wide = scores.unstack(level=1).reindex(index=returns.index, columns=returns.columns)

    dates = returns.index
    held = pd.Series(dtype=float)
    rows: dict[pd.Timestamp, pd.Series] = {}
    realized: dict[pd.Timestamp, float] = {}
    turnover: dict[pd.Timestamp, float] = {}
    gross: dict[pd.Timestamp, float] = {}
    rebalances = 0
    forced_exits = 0
    unrepaired = 0
    thin = 0
    delisting_loss = 0.0

    scored_dates = score_wide.notna().any(axis=1)
    first_scored = scored_dates.idxmax() if scored_dates.any() else None
    if first_scored is None:
        raise ValueError("no usable scores: the pipeline produced nothing to trade")
    start = int(dates.get_loc(first_scored))

    for position in range(start, len(dates)):
        date = dates[position]
        members = returns.loc[date].dropna().index
        available = score_wide.loc[date, members].dropna()

        # Yesterday's book, and what the universe did to it overnight. Departures
        # are closed whether or not a rebalance was due, and the cost of closing
        # them is turnover that was never anybody's choice.
        book = held
        departed = book.index.difference(members)
        closed = float(book.reindex(departed).abs().sum()) if len(departed) else 0.0
        forced_exits += int(len(departed))
        surviving = book.reindex(book.index.intersection(members))

        # A departure is a rebalance trigger of its own. Holding a book from
        # which the universe has just removed a name means holding one that no
        # longer sums to zero, is no longer sector neutral and no longer has the
        # beta it was built with -- so a schedule that ignored departures would
        # produce books that violate the constraints this module advertises,
        # between rebalances, without saying so.
        # Too few scored names to make a compliant book is a normal condition, not
        # an error: the last `horizon` dates of a run have labels only for the
        # names that ended early, and a fold whose training set was entirely
        # purged contributes no predictions at all. The book holds rather than
        # being rebuilt out of three names, and the date is counted.
        buildable = not available.empty and constraints.feasible_for(len(available))
        due = ((position - start) % rebalance_every == 0) or book.empty or len(departed) > 0
        if due and not buildable:
            thin += 1

        if due and buildable:
            held = build_weights(available, betas, sectors, constraints,
                                 previous=surviving if not surviving.empty else None)
            rebalances += 1
        else:
            held = surviving
            if len(departed):
                unrepaired += 1

        rows[date] = held.copy()
        gross[date] = float(held.abs().sum())
        combined = held.index.union(surviving.index)
        turnover[date] = closed + float((held.reindex(combined).fillna(0.0)
                                         - surviving.reindex(combined).fillna(0.0)).abs().sum())

        if position + 1 < len(dates):
            tomorrow = dates[position + 1]
            # The names the book can still earn on tomorrow. A name that leaves
            # *on* tomorrow is still there tomorrow -- its final period carries
            # the departure return, and a position in it earns that return. That
            # is the whole point of the forced-exit accounting: the loss is
            # realized, not skipped.
            earning = held.index.intersection(returns.loc[tomorrow].dropna().index)
            period = float((held.reindex(earning) * returns.loc[tomorrow, earning]).sum())
            realized[tomorrow] = period

            dying = earning[delisted.loc[tomorrow, earning].to_numpy(dtype=bool)]
            if len(dying):
                delisting_loss += float((held.reindex(dying)
                                         * returns.loc[tomorrow, dying]).sum())

    return PortfolioResult(
        weights=pd.DataFrame(rows).T.sort_index(),
        active_returns=pd.Series(realized, dtype=float).sort_index(),
        turnover=pd.Series(turnover, dtype=float).sort_index(),
        gross=pd.Series(gross, dtype=float).sort_index(),
        rebalances=rebalances,
        forced_exits=forced_exits,
        unrepaired=unrepaired,
        thin=thin,
        delisting_loss=delisting_loss,
    )
