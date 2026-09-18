"""M5: every portfolio constraint, violated on purpose and confirmed rejected.

Two halves. The first builds books and asserts they comply. The second takes a
compliant book, breaks one constraint at a time by hand, and asserts the
validator names it -- because a validator that has only ever been shown valid
input is an assertion that has never been tested.
"""

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from research.panel import PanelSpec, generate_panel
from research.portfolio import (
    ConstraintError,
    PortfolioConstraints,
    build_portfolio,
    build_weights,
    violations,
)


@pytest.fixture(scope="module")
def panel():
    return generate_panel(PanelSpec(n_securities=80, n_periods=260, lam=0.001, seed=71))


@pytest.fixture(scope="module")
def one_date(panel):
    """Scores, betas and sectors for a single cross-section."""
    date = panel.dates[100]
    scores = panel.frame.loc[date, "exposure"]
    return scores, panel.betas, panel.sectors


def test_a_built_book_satisfies_every_constraint(one_date):
    scores, betas, sectors = one_date
    constraints = PortfolioConstraints(gross=1.0, max_weight=0.03, turnover_budget=np.inf)
    weights = build_weights(scores, betas, sectors, constraints)

    assert violations(weights, betas, sectors, constraints) == []
    assert weights.abs().sum() == pytest.approx(1.0, abs=1e-3)
    assert weights.abs().max() <= 0.03 + 1e-9


def test_the_cap_binds_when_it_is_tight(one_date):
    scores, betas, sectors = one_date
    constraints = PortfolioConstraints(gross=1.0, max_weight=0.015, turnover_budget=np.inf)
    weights = build_weights(scores, betas, sectors, constraints)

    assert violations(weights, betas, sectors, constraints) == []
    pinned = (weights.abs() > 0.015 - 1e-6).sum()
    assert pinned > 0, "a cap this tight should pin some names; it did not bind at all"


def test_a_nonzero_beta_target_is_hit(one_date):
    scores, betas, sectors = one_date
    constraints = PortfolioConstraints(gross=1.0, max_weight=0.05, beta_target=0.10,
                                       turnover_budget=np.inf)
    weights = build_weights(scores, betas, sectors, constraints)

    assert violations(weights, betas, sectors, constraints) == []
    assert float((weights * betas.reindex(weights.index)).sum()) == pytest.approx(0.10, abs=1e-5)


def test_sector_neutrality_can_be_turned_off_but_the_book_still_nets_zero(one_date):
    scores, betas, sectors = one_date
    constraints = PortfolioConstraints(sector_neutral=False, max_weight=0.05,
                                       turnover_budget=np.inf)
    weights = build_weights(scores, betas, sectors, constraints)

    assert violations(weights, betas, sectors, constraints) == []
    assert float(weights.sum()) == pytest.approx(0.0, abs=1e-6)

    by_sector = weights.groupby(sectors.reindex(weights.index)).sum()
    assert by_sector.abs().max() > 1e-5, ("with neutrality off the sectors happened to net to "
                                          "zero anyway; this test is not testing it")


def test_the_turnover_budget_binds(one_date):
    scores, betas, sectors = one_date
    loose = PortfolioConstraints(max_weight=0.05, turnover_budget=np.inf)
    start = build_weights(scores, betas, sectors, loose)

    # A different cross-section: the target book is a long way from `start`.
    flipped = -scores
    tight = PortfolioConstraints(max_weight=0.05, turnover_budget=0.10)
    moved = build_weights(flipped, betas, sectors, tight, previous=start)

    turnover = float((moved - start.reindex(moved.index).fillna(0.0)).abs().sum())
    assert turnover <= 0.10 + 1e-6
    assert violations(moved, betas, sectors, tight, previous=start) == []

    # And it really was held back: without the budget it would have moved much
    # further. A budget that binds only in arithmetic is not a budget.
    unbounded = build_weights(flipped, betas, sectors, loose, previous=start)
    assert float((unbounded - start).abs().sum()) > 0.5


def test_repeated_rebalancing_on_a_binding_budget_stays_compliant(one_date):
    """A property check, and an honest note about what it is not.

    A twenty-year sweep hit a rebalance where the projection cycle ran out of
    iterations five parts in a hundred thousand off neutral. The cause was the
    stopping rule: it broke when the iterate stopped *moving*, and alternating
    projections onto sets meeting at a shallow angle take small steps long before
    they arrive. The intersection was not empty -- a thousand iterations reached
    it -- so the book was reachable and the loop gave up early and returned it
    anyway. The rule now requires the equalities to hold as well.

    **This test does not reproduce that failure.** The state took thousands of
    rebalances on a real panel to reach, and it survived every attempt to shrink
    it: random targets, tied scores standing in for a shallow tree's leaves,
    four hundred-step chains at the exact parameters. What is asserted here is
    the property that was violated, not the path to violating it.

    The thing that actually caught it was the builder validating its own output.
    That is the guard worth keeping.
    """
    scores, betas, sectors = one_date
    constraints = PortfolioConstraints(gross=1.0, max_weight=0.04, turnover_budget=0.25)
    book = build_weights(scores, betas, sectors, constraints)

    rng = np.random.default_rng(11)
    for _ in range(25):
        shuffled = pd.Series(rng.permutation(scores.to_numpy()), index=scores.index)
        book = build_weights(shuffled, betas, sectors, constraints, previous=book)
        assert violations(book, betas, sectors, constraints) == []


def test_infeasible_constraints_are_rejected_rather_than_approximated(one_date):
    """Ten names at a 2% cap cannot make up a gross of 1.0. Say so."""
    scores, betas, sectors = one_date
    constraints = PortfolioConstraints(gross=1.0, max_weight=0.02)
    with pytest.raises(ConstraintError, match="infeasible"):
        build_weights(scores.iloc[:10], betas, sectors, constraints)


# ---------------------------------------------------------------------------
# The other half: break each constraint and confirm the validator says so.
# ---------------------------------------------------------------------------


@pytest.fixture
def compliant(one_date):
    scores, betas, sectors = one_date
    constraints = PortfolioConstraints(gross=1.0, max_weight=0.04, turnover_budget=np.inf)
    return build_weights(scores, betas, sectors, constraints), betas, sectors, constraints


def test_violating_the_sum_to_zero_is_caught(compliant):
    weights, betas, sectors, constraints = compliant
    broken = weights.copy()
    broken.iloc[0] += 0.01
    assert any("sum to zero" in message for message in
               violations(broken, betas, sectors, constraints))


def test_violating_the_gross_is_caught(compliant):
    weights, betas, sectors, constraints = compliant
    broken = weights * 3.0
    assert any("gross" in message for message in violations(broken, betas, sectors, constraints))


def test_violating_the_cap_is_caught(compliant):
    weights, betas, sectors, constraints = compliant
    broken = weights.copy()
    # Moved in a pair so that the book still nets to zero: the cap has to be
    # caught on its own, not as a side effect of another violation.
    broken.iloc[0] = constraints.max_weight * 2.0
    broken.iloc[1] = broken.iloc[1] - constraints.max_weight
    assert any("max weight" in message for message in
               violations(broken, betas, sectors, constraints))


def test_violating_sector_neutrality_is_caught(compliant):
    weights, betas, sectors, constraints = compliant
    sector_of = sectors.reindex(weights.index)
    first = sector_of[sector_of == sector_of.iloc[0]].index[0]
    other = sector_of[sector_of != sector_of.iloc[0]].index[0]

    broken = weights.copy()
    broken[first] += 0.005
    broken[other] -= 0.005  # still sums to zero overall
    messages = violations(broken, betas, sectors, constraints)
    assert any("sector" in message for message in messages)
    assert not any("sum to zero" in message for message in messages)


def test_violating_the_beta_target_is_caught(compliant):
    weights, betas, sectors, constraints = compliant
    aligned = betas.reindex(weights.index)
    high = aligned.idxmax()
    low = aligned.idxmin()

    broken = weights.copy()
    broken[high] += 0.01
    broken[low] -= 0.01  # zero-sum, same sector count, different beta
    messages = violations(broken, betas, sectors, constraints)
    assert any("active beta" in message for message in messages)


def test_violating_the_turnover_budget_is_caught(compliant):
    weights, betas, sectors, _ = compliant
    constraints = PortfolioConstraints(gross=1.0, max_weight=0.04, turnover_budget=0.01)
    assert any("turnover" in message for message in
               violations(weights, betas, sectors, constraints, previous=-weights))


def test_non_finite_weights_are_caught(compliant):
    weights, betas, sectors, constraints = compliant
    broken = weights.copy()
    broken.iloc[0] = np.nan
    assert violations(broken, betas, sectors, constraints) == ["non-finite weights"]


# ---------------------------------------------------------------------------
# The book over time
# ---------------------------------------------------------------------------


def test_every_book_over_a_whole_run_is_compliant(panel):
    """The builder validates each book as it makes it, so a run that completes is
    itself the assertion. This test exists to make that explicit, and to catch a
    future change that stops validating."""
    scores = panel.frame["exposure"]
    result = build_portfolio(scores, panel.frame, panel.betas, panel.sectors,
                             constraints=PortfolioConstraints(max_weight=0.05),
                             rebalance_every=21)
    assert result.rebalances > 5
    assert result.unrepaired == 0

    for date, row in result.weights.iterrows():
        book = row.dropna()
        if book.empty:
            continue
        assert float(book.sum()) == pytest.approx(0.0, abs=1e-5)
        assert book.abs().max() <= 0.05 + 1e-6


def test_weights_are_held_between_rebalances(panel):
    scores = panel.frame["exposure"]
    result = build_portfolio(scores, panel.frame, panel.betas, panel.sectors,
                             rebalance_every=21)
    traded = (result.turnover > 1e-9).sum()
    assert traded < len(result.turnover) / 2, "the book traded on most dates; it is not being held"


def test_a_departure_closes_the_position_and_the_loss_is_realized():
    """The delisting must reach the return series, not vanish with the name.

    A held position in a name that leaves earns the departure return in its final
    period. That is the loss naive backtests lose -- they liquidate at the last
    price, which is the price *before* the drop.
    """
    panel = generate_panel(PanelSpec(n_securities=60, n_periods=300, lam=0.001, seed=73,
                                     delist_hazard=0.004, delist_exposure_tilt=0.0,
                                     delist_return=-0.5))
    # Score every name by exposure so the book is long the high ones; with no
    # hazard tilt, departures are indifferent to the score and some will be longs.
    result = build_portfolio(panel.frame["exposure"], panel.frame, panel.betas, panel.sectors,
                             constraints=PortfolioConstraints(max_weight=0.05),
                             rebalance_every=5)

    assert result.forced_exits > 0
    assert result.delisting_loss != 0.0, "no position was ever held into a departure"

    # No name survives in the book after it leaves the universe.
    members = panel.frame.index.to_frame(index=False).groupby("security")["date"].max()
    for security in result.weights.columns:
        held = result.weights[security].dropna()
        held = held[held.abs() > 0]
        if held.empty:
            continue
        assert held.index.max() <= members[security]


def test_turnover_includes_the_trade_the_universe_forced(panel):
    churning = generate_panel(PanelSpec(n_securities=60, n_periods=300, seed=74,
                                        delist_hazard=0.004))
    result = build_portfolio(churning.frame["exposure"], churning.frame, churning.betas,
                             churning.sectors,
                             constraints=PortfolioConstraints(max_weight=0.05),
                             rebalance_every=21)
    assert result.forced_exits > 0
    # Departures force a rebuild, so there are more rebalances than the schedule
    # alone would produce.
    scheduled = len(result.turnover) // 21 + 1
    assert result.rebalances > scheduled


def test_an_empty_score_set_is_an_error_not_an_empty_book(panel):
    empty = panel.frame["exposure"] * np.nan
    with pytest.raises(ValueError, match="no usable scores"):
        build_portfolio(empty, panel.frame, panel.betas, panel.sectors)
