"""M7: each pathology asserts a quantitative consequence.

The panels here are smaller than the ones the committed report is generated from,
because a test is a gate and the report is the result. What the two share is the
shape of the assertion: a direction and a magnitude, never "it ran".
"""

from __future__ import annotations

import pytest

from research.features import Feature, exposure_feature
from research.labels import forward_return
from research.panel import PanelSpec, generate_panel
from research.delisting import APPLY_TRUE_RETURN, applied_returns
from research.pathology import (
    HONEST_R2_CEILING,
    measure_delisting_flattery,
    benchmark_return,
    measure_delisting,
    measure_lookahead,
    measure_survivorship,
    survivors_only,
)
from research.pathology_report import write_report
from research.pipeline import PipelineConfig, run_pipeline

SMALL = PanelSpec(n_securities=120, n_periods=700, lam=0.0008, seed=20260918,
                  delist_hazard=0.0015, delist_exposure_tilt=-0.8)


# ---------------------------------------------------------------------------
# 1. Survivorship
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def survivorship():
    return measure_survivorship(SMALL, PipelineConfig(build_portfolio=False,
                                                      delisting_model=APPLY_TRUE_RETURN))


def test_survivors_only_drops_exactly_the_names_that_left():
    panel = generate_panel(SMALL)
    biased = survivors_only(panel)

    assert len(biased.securities) < len(panel.securities)
    assert set(biased.securities) == set(panel.survivors)
    assert not biased.frame["delisted"].any(), "a survivor cannot have departed"

    # And it keeps the survivors' whole history, which is the bias itself: rows
    # from before anybody could have known they would survive.
    assert biased.frame.index.get_level_values(0).min() == panel.dates[0]


def test_the_index_looks_better_without_the_failures():
    """The level effect. This direction is not configuration-dependent.

    With the delisting return applied, which it has to be: Session D moved it out
    of the price series, so under `last_price` a failure costs nothing and this
    measures the wrong thing. It did, briefly, and said so -- it reported the
    survivors-only index returning *less*, which is the only claim in the report
    whose sign is not negotiable.
    """
    panel = generate_panel(SMALL)
    applied = applied_returns(panel.delistings, APPLY_TRUE_RETURN)
    honest = benchmark_return(panel, applied)
    biased = benchmark_return(survivors_only(panel), applied)

    assert biased > honest
    assert biased - honest > 0.005, (f"survivorship moved the index return by only "
                                     f"{biased - honest:.4%} a year; too small to measure")


def test_the_premium_inflates_when_the_failures_were_in_the_long_leg(survivorship):
    """M7's first definition of done: a material inflation, with its magnitude."""
    finding, detail = survivorship

    assert finding.pathological > finding.honest
    assert finding.inflation > 1.10, f"survivorship was worth only {finding.inflation:.2f}x"
    # Material against the estimate's own error bars, not just against zero.
    assert finding.pathological - finding.honest > 2.0 * detail["stderr"]


def test_the_premium_deflates_when_the_failures_were_in_the_short_leg(survivorship):
    """The counterpart, and the reason the report refuses to state a sign.

    If this ever fails, it means the direction of survivorship bias stopped
    depending on which leg the failures were in -- which would be a finding, and
    would make the report wrong.
    """
    _, detail = survivorship
    assert detail["mirrored_biased"] < detail["mirrored_honest"]


def test_a_panel_nobody_leaves_is_refused():
    with pytest.raises(ValueError, match="nobody leaves"):
        measure_survivorship(SMALL.replace(delist_hazard=0.0))


# ---------------------------------------------------------------------------
# 2. Lookahead
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def lookahead():
    return measure_lookahead(PanelSpec(n_securities=100, n_periods=700, lam=0.0005,
                                       seed=20260919))


def test_a_leaking_feature_clears_a_threshold_no_honest_one_reaches(lookahead):
    """M7's second definition of done."""
    finding, _ = lookahead

    assert finding.honest < HONEST_R2_CEILING, ("the honest pipeline already exceeds the "
                                                "ceiling; the threshold is wrong")
    assert finding.pathological > HONEST_R2_CEILING
    assert finding.pathological > 20.0 * max(finding.honest, 1e-4)


def test_a_partial_overlap_leaks_less_but_still_leaks(lookahead):
    """The realistic case: visible, believable, and still wrong."""
    _, detail = lookahead

    assert detail["partial_leaked_r2"] > detail["partial_honest_r2"]
    assert detail["partial_leaked_ic"] > detail["partial_honest_ic"]
    assert detail["partial_leaked_r2"] < detail["partial_honest_r2"] + 1.0


def test_the_leak_is_reported_as_a_leak():
    panel = generate_panel(PanelSpec(n_securities=40, n_periods=200, seed=20260921))
    result = run_pipeline(panel, PipelineConfig(
        features=(exposure_feature(),
                  Feature(source="ret", transform="leak", window=1)),
        build_portfolio=False))

    assert result.leaking
    assert "not honest" in result.report()


# ---------------------------------------------------------------------------
# 3. Delisting
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def delisting():
    return measure_delisting(PanelSpec(n_securities=100, n_periods=700, lam=0.0005,
                                       seed=20260920, delist_hazard=0.0015,
                                       delist_exposure_tilt=0.0))


def test_dropping_truncated_labels_removes_the_loss(delisting):
    """M7's third definition of done, on the pipeline side."""
    finding, detail = delisting

    assert detail["labels_dropped"] > 0
    assert detail["truncated_label_return"] < 0.0
    assert finding.pathological > finding.honest, "dropping the losses did not flatter the study"
    assert detail["worst_label_compound"] <= -0.99, "a -100% departure never reached a label"


def test_dropping_removes_most_of_the_loss_and_leaves_the_worst_case_standing(delisting):
    """What the naive policy actually removes, and what it conspicuously does not.

    Of the `horizon` labels whose window contains a departure, exactly one is
    untruncated -- the window ending precisely on the departure date -- and it
    survives both policies. So dropping removes most of the loss *mass* while
    leaving the sample minimum untouched, which is the failure mode worth naming:
    a study that checks its own worst observation finds it intact and concludes
    the delistings are being counted.

    Written the other way round first -- that the surviving rows were the ones
    further back -- and the measurement said otherwise.
    """
    _, detail = delisting

    assert detail["worst_label_compound"] == detail["worst_label_drop"]
    assert detail["mean_label_drop"] > detail["mean_label_compound"]

    ruinous_before = detail["ruinous_labels_compound"]
    ruinous_after = detail["ruinous_labels_drop"]
    assert 0 < ruinous_after < ruinous_before
    assert ruinous_after < 0.25 * ruinous_before, (
        f"{ruinous_after:.0f} of {ruinous_before:.0f} ruinous labels survived the drop; "
        f"expected roughly one per departure")


def test_the_book_realizes_the_delisting_rather_than_stepping_around_it(delisting):
    _, detail = delisting
    assert detail["forced_exits"] > 0
    assert detail["portfolio_delisting_return_compound"] != 0.0


def test_the_flattery_of_a_zero_delisting_return_is_material():
    """Session D Part 2.3, as a gate.

    Assuming a position was sold at the last print, when it was not, has to move
    the result by an amount nobody could call second-order -- and it has to move
    it in the flattering direction when the book is long the names that fail.
    """
    finding, detail = measure_delisting_flattery()

    assert detail["flattery_return"] > 0.05, "assuming a zero delisting return barely mattered"
    assert detail["flattery_sharpe"] > 0.5
    assert finding.pathological > finding.honest

    # And the mirror: short the failures and the same assumption understates.
    assert detail["mirror_return"] < 0.0


def test_a_held_position_earns_the_departure_return():
    """The mechanism, in the smallest case that shows it.

    Not a statistical claim: one name, one departure, and the arithmetic that the
    position was still on when the -100% landed.
    """
    panel = generate_panel(PanelSpec(n_securities=40, n_periods=300, seed=20260922,
                                     delist_hazard=0.004))
    departures = panel.frame[panel.frame["delisted"]]
    assert not departures.empty

    applied = applied_returns(panel.delistings, APPLY_TRUE_RETURN)
    labels = forward_return(panel.frame, horizon=5, terminal="compound",
                            delisting_returns=applied)
    truncated = labels.values[labels.truncated]
    assert not truncated.empty
    assert truncated.min() <= -0.9

    dropped = forward_return(panel.frame, horizon=5, terminal="drop",
                             delisting_returns=applied)
    assert not dropped.truncated.any()
    assert len(dropped.values) < len(labels.values)


def test_a_panel_with_no_departures_is_refused():
    with pytest.raises(ValueError, match="no departures"):
        measure_delisting(PanelSpec(n_securities=20, n_periods=100, delist_hazard=0.0))


# ---------------------------------------------------------------------------
# The report
# ---------------------------------------------------------------------------


def test_the_report_is_written_from_the_measurements(survivorship, lookahead, delisting,
                                                     tmp_path):
    assumed = measure_delisting_flattery()
    findings = {"survivorship": survivorship[0], "lookahead": lookahead[0],
                "delisting": delisting[0], "assumed_exit": assumed[0]}
    details = {"survivorship": survivorship[1], "lookahead": lookahead[1],
               "delisting": delisting[1], "assumed_exit": assumed[1]}

    path = write_report(findings, details, tmp_path / "PATHOLOGIES.md")
    text = path.read_text(encoding="utf-8")

    assert "# Pathologies" in text
    assert "statement about this pipeline on synthetic data" in text
    # By the finding's own display name, not the dictionary key: "assumed_exit"
    # is how the code addresses it and "assumed exit" is what a reader sees.
    for finding in findings.values():
        assert finding.name in text
    # The numbers in the prose come from the measurements, so a stale report
    # cannot survive a change in what was measured.
    assert f"{findings['survivorship'].inflation:.2f}x" in text
    assert "Nothing in this file has run the C++ ledger." in text
