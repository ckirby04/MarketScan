"""M7: three biases, injected deliberately, and what each one is worth.

    python -m research.pathology            # runs all three, writes docs/PATHOLOGIES.md

Everything up to here established that the pipeline finds what was planted. This
milestone establishes the other half: that it can be fooled, that the guards
against being fooled do something, and -- the part that is actually useful -- how
large the errors are in the units this project reports its results in.

The three are the ones that account for most wrong backtests:

**Survivorship.** Build the universe from the names that were still there at the
end. The failures leave with them, and both the index's own return and the
measured premium change. The premium's *direction* depends on which leg the
failures were in, which is why it is measured both ways; the index level only
ever moves one way. By how much, in each case, is the number.

**Lookahead.** Put tomorrow's return into today's feature. This is not subtle and
it is not meant to be: the interesting output is the *smell* -- what an
out-of-sample R-squared looks like when a feature is leaking, so that a number of
that size is recognised on sight rather than celebrated.

**Delisting.** A name goes to -100% and leaves. A pipeline that labels forward
returns naively has no label for the dates leading into that, so the loss is
absent from the study: not survivorship exactly, but the same money vanishing by
a different route. `labels.py` has both policies, and the difference between them
is the measurement.

WHAT THIS DOES NOT MEASURE

The delisting section covers the research pipeline's half. The engine's half --
that a held position is force-liquidated, logged distinctly, and credited to cash
-- is asserted by the C++ suite (`tests/test_universe.cpp`, "a held security that
leaves the universe is liquidated") and is re-checked against this pipeline at M9,
where the same strategy runs both ways and the results have to agree. Nothing here
has run the C++ ledger, and this file does not claim otherwise.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from research import delisting, metrics
from research.features import Feature, exposure_feature
from research.labels import forward_return
from research.panel import Panel, PanelSpec, generate_panel
from research.delisting import APPLY_TRUE_RETURN, LAST_PRICE
from research.pipeline import PipelineConfig, run_pipeline

REPO = Path(__file__).resolve().parent.parent

SURVIVORSHIP_SPEC = PanelSpec(
    n_securities=200,
    n_periods=1250,
    lam=0.0005,
    seed=20260918,
    delist_hazard=0.0008,
    delist_exposure_tilt=-0.8,
)
"""Roughly five percent of the panel leaves per year, and the departures are
tilted towards names with a *high* exposure -- the ones the signal liked.

That sign is the one the classic result is about, and it is worth being explicit
about why, because the first run of this milestone used the opposite tilt and
measured survivorship bias making the premium *smaller*. Both are real. A
long-short book is long high exposure and short low exposure, so:

- deaths among high-exposure names are losses the book took. Remove them and the
  premium inflates -- this spec, and the reason a study of "the names with
  characteristic X did well" over today's index members is not evidence.
- deaths among low-exposure names are gains the book made by being short them.
  Remove those and the premium *deflates*.

The sign of survivorship bias follows the correlation between failure and the
signal. What is reliable is the size, and that it is large. Both directions are
measured below.

Not calibrated against any real index -- the rate was chosen to make the effect
measurable in a panel of this length, and every magnitude here is a statement
about this panel."""

HONEST_R2_CEILING = 0.02
"""No honest daily cross-sectional feature reaches this out of sample.

A 2% out-of-sample R-squared on daily returns would be an information coefficient
north of 0.14 sustained across a whole panel. The literature's honest numbers are
an order of magnitude below it. The lookahead test asserts the leaking pipeline
clears this, which is the assertion that the smell is unmistakable.
"""


@dataclass(frozen=True)
class Finding:
    """One pathology, its magnitude, and the number it should be compared to."""

    name: str
    honest: float
    pathological: float
    units: str
    note: str

    @property
    def inflation(self) -> float:
        """Pathological over honest. NaN when the honest value is zero."""
        if not np.isfinite(self.honest) or self.honest == 0.0:
            return metrics.UNDEFINED
        return self.pathological / self.honest

    def __str__(self) -> str:
        ratio = self.inflation
        tail = f" ({ratio:.2f}x)" if np.isfinite(ratio) else ""
        return (f"{self.name}: honest {self.honest:+.6f}, "
                f"pathological {self.pathological:+.6f} {self.units}{tail}")


# ---------------------------------------------------------------------------
# 1. Survivorship
# ---------------------------------------------------------------------------


def survivors_only(panel: Panel) -> Panel:
    """The same panel as a study that built its universe from today's members.

    Every row of every name that was still a member on the final date, and no row
    of any name that was not. Note what this keeps: the *full history* of the
    survivors, including their history from before anybody could have known they
    would survive. That is the bias, stated exactly.
    """
    survivors = panel.survivors
    frame = panel.frame.loc[panel.frame.index.get_level_values(1).isin(survivors)].copy()
    membership = panel.membership[panel.membership["security"].isin(survivors)]
    return Panel(
        spec=panel.spec,
        frame=frame,
        market=panel.market,
        betas=panel.betas.reindex(survivors),
        sectors=panel.sectors.reindex(survivors),
        membership=membership.reset_index(drop=True),
        delistings=panel.delistings.reindex(
            panel.delistings.index.intersection(survivors)),
    )


def benchmark_return(panel: Panel,
                     delisting_returns: dict[str, float] | None = None) -> float:
    """Annualized mean return of an equal-weight book over whoever was a member.

    The level effect, and the one part of survivorship bias whose sign is not up
    for discussion: the survivors are the names that did not fall over, so the
    index they make up always looks better than the one anybody could have held.

    The departure return has to be in here, and once Session D moved it out of
    the price series it had to be put back by hand. Without it this measured the
    wrong thing and said so loudly: it reported the survivors-only index
    returning *less* than the point-in-time one, which is the opposite of the
    only claim in this document whose sign is not negotiable. What it was
    actually measuring was that the hazard is tilted towards high-exposure names
    and high-exposure names earn more -- a real effect, and not this one.
    """
    returns = panel.frame["ret"].copy()
    if delisting_returns:
        departures = panel.frame.index[panel.frame["delisted"].to_numpy(dtype=bool)]
        for date, security in departures:
            extra = delisting_returns.get(str(security))
            if extra is not None:
                base = float(returns.loc[(date, security)])
                returns.loc[(date, security)] = (1.0 + base) * (1.0 + extra) - 1.0
    per_date = returns.groupby(level=0).mean()
    return float(per_date.mean() * metrics.ANNUAL_PERIODS)


def measure_survivorship(spec: PanelSpec = SURVIVORSHIP_SPEC,
                         config: PipelineConfig | None = None) -> tuple[Finding,
                                                                       dict[str, float]]:
    """The premium measured point-in-time, against the same panel's survivors.

    Runs it twice, with the hazard tilted each way, because the direction of the
    bias is a property of which leg the failures were in and not of survivorship
    itself. See the note on `SURVIVORSHIP_SPEC`.
    """
    # Measured with the delisting return applied. Survivorship bias is about
    # losing the failures, and since Session D moved the departure return out of
    # the price series, a failure under `last_price` costs nothing at all -- so
    # the bias would have nothing to hide and the measurement would be of noise.
    config = config or PipelineConfig(build_portfolio=False,
                                      delisting_model=APPLY_TRUE_RETURN)
    if not spec.delist_hazard:
        raise ValueError("a panel nobody leaves cannot demonstrate survivorship bias")

    panel = generate_panel(spec)
    applied = delisting.applied_returns(panel.delistings, config.delisting_model)
    honest = run_pipeline(panel, config)
    biased = run_pipeline(survivors_only(panel), config)

    mirrored_spec = spec.replace(delist_exposure_tilt=-spec.delist_exposure_tilt)
    mirrored = generate_panel(mirrored_spec)
    mirrored_honest = run_pipeline(mirrored, config)
    mirrored_biased = run_pipeline(survivors_only(mirrored), config)

    detail = {
        "departures": float(panel.frame["delisted"].sum()),
        "names_point_in_time": float(len(panel.securities)),
        "names_survivors_only": float(len(panel.survivors)),
        "benchmark_point_in_time": benchmark_return(panel, applied),
        "benchmark_survivors_only": benchmark_return(survivors_only(panel), applied),
        "ic_point_in_time": honest.ic_mean,
        "ic_survivors_only": biased.ic_mean,
        "mirrored_tilt": mirrored_spec.delist_exposure_tilt,
        "mirrored_honest": mirrored_honest.lambda_hat.estimate,
        "mirrored_biased": mirrored_biased.lambda_hat.estimate,
        "stderr": honest.lambda_hat.stderr,
    }

    return Finding(
        name="survivorship",
        honest=honest.lambda_hat.estimate,
        pathological=biased.lambda_hat.estimate,
        units="per period, per unit exposure",
        note=(f"{int(detail['departures'])} departures removed; "
              f"{int(detail['names_point_in_time'])} names seen point-in-time, "
              f"{int(detail['names_survivors_only'])} kept by the survivors-only universe. "
              f"IC {honest.ic_mean:+.4f} -> {biased.ic_mean:+.4f}"),
    ), detail


# ---------------------------------------------------------------------------
# 2. Lookahead
# ---------------------------------------------------------------------------


def measure_lookahead(spec: PanelSpec | None = None) -> tuple[Finding, dict[str, float]]:
    """One leaking feature against the same pipeline without it.

    Two leaks, not one. The headline is the gross case -- tomorrow's return, which
    is the whole label -- and it is included because a threshold has to be set
    somewhere obvious. The one that teaches more is the partial case: a ten-period
    label and a feature that has seen only the fifth of those ten periods. That is
    what a real leak looks like -- a window that overlaps a label by an amount
    nobody added up -- and it is the reason `splits.py` purges on label windows
    rather than on dates.
    """
    spec = spec or PanelSpec(n_securities=150, n_periods=1000, lam=0.0005, seed=20260919)
    panel = generate_panel(spec)

    honest = run_pipeline(panel, PipelineConfig(features=(exposure_feature(),),
                                                build_portfolio=False))
    leaked = run_pipeline(panel, PipelineConfig(
        features=(exposure_feature(),
                  Feature(source="ret", transform="leak", window=1, cross_section="zscore")),
        build_portfolio=False))
    assert leaked.leaking, "the leaking feature was not flagged as leaking"

    partial_honest = run_pipeline(panel, PipelineConfig(
        features=(exposure_feature(),), horizon=10, embargo=5, build_portfolio=False))
    partial_leaked = run_pipeline(panel, PipelineConfig(
        features=(exposure_feature(),
                  Feature(source="ret", transform="leak", window=5, cross_section="zscore")),
        horizon=10, embargo=5, build_portfolio=False))

    detail = {
        "honest_ic": honest.ic_mean,
        "leaked_ic": leaked.ic_mean,
        "honest_ic_t": honest.ic_t,
        "leaked_ic_t": leaked.ic_t,
        "partial_honest_r2": partial_honest.oos_r2,
        "partial_leaked_r2": partial_leaked.oos_r2,
        "partial_honest_ic": partial_honest.ic_mean,
        "partial_leaked_ic": partial_leaked.ic_mean,
    }

    return Finding(
        name="lookahead",
        honest=honest.oos_r2,
        pathological=leaked.oos_r2,
        units="out-of-sample R-squared",
        note=(f"IC {honest.ic_mean:+.4f} -> {leaked.ic_mean:+.4f}; "
              f"t {honest.ic_t:+.1f} -> {leaked.ic_t:+.1f}. "
              f"One feature, one period of lookahead."),
    ), detail


# ---------------------------------------------------------------------------
# 3. Delisting
# ---------------------------------------------------------------------------


def measure_delisting(spec: PanelSpec | None = None) -> tuple[Finding, dict[str, float]]:
    """A name goes to -100%, and the study either sees it or does not.

    Returns the finding and the portfolio-level detail: what a book holding those
    names earned in their final period under each labelling policy.
    """
    spec = spec or PanelSpec(n_securities=150, n_periods=1000, lam=0.0005, seed=20260920,
                             delist_hazard=0.0012, delist_exposure_tilt=0.0)
    panel = generate_panel(spec)
    departures = int(panel.frame["delisted"].sum())
    if departures == 0:
        raise ValueError("no departures were generated; nothing to measure")

    horizon = 10
    # With the delisting return applied, so there is a loss for the naive label
    # policy to drop. Under `last_price` a departure costs nothing and this
    # measurement would have nothing to measure.
    applied = delisting.applied_returns(panel.delistings, APPLY_TRUE_RETURN)
    compounded = run_pipeline(panel, PipelineConfig(horizon=horizon, terminal="compound",
                                                    embargo=5,
                                                    delisting_model=APPLY_TRUE_RETURN))
    dropped = run_pipeline(panel, PipelineConfig(horizon=horizon, terminal="drop", embargo=5,
                                                 delisting_model=APPLY_TRUE_RETURN))

    seen = forward_return(panel.frame, horizon=horizon, terminal="compound",
                          delisting_returns=applied)
    unseen = forward_return(panel.frame, horizon=horizon, terminal="drop",
                            delisting_returns=applied)
    lost_rows = len(seen.values) - len(unseen.values)
    lost_return = float(seen.values[seen.truncated].sum())

    # The headline is the mean label, not the estimated lam. With the hazard
    # untilted the deaths are unrelated to the exposure, so what they do to a
    # cross-sectional *slope* is mostly noise -- a -100% label at a random
    # exposure moves a per-date slope in a random direction, and reading a ratio
    # off two noisy estimates would be reporting the seed. What they do to the
    # *level* of the study's returns is systematic and is the thing that vanishes:
    # drop the truncated labels and the average forward return in the sample rises
    # by the whole weight of the losses that were removed.
    detail = {
        "departures": float(departures),
        "labels_dropped": float(lost_rows),
        "truncated_label_return": lost_return,
        "mean_label_compound": float(seen.values.mean()),
        "mean_label_drop": float(unseen.values.mean()),
        "worst_label_compound": float(seen.values.min()),
        "worst_label_drop": float(unseen.values.min()),
        # Labels that swallowed a departure whole. Counted rather than reasoned
        # about, because the first version of this report reasoned about it and
        # got it wrong: of the `horizon` labels that contain a given death, only
        # the one whose window ends exactly on the departure date is untruncated.
        # The naive policy therefore drops all but one of them.
        "ruinous_labels_compound": float((seen.values <= -0.99).sum()),
        "ruinous_labels_drop": float((unseen.values <= -0.99).sum()),
        "lambda_compound": compounded.lambda_hat.estimate,
        "lambda_drop": dropped.lambda_hat.estimate,
        "lambda_stderr": compounded.lambda_hat.stderr,
        "portfolio_delisting_return_compound":
            compounded.portfolio.delisting_loss if compounded.portfolio else metrics.UNDEFINED,
        "portfolio_delisting_return_drop":
            dropped.portfolio.delisting_loss if dropped.portfolio else metrics.UNDEFINED,
        "forced_exits": float(compounded.portfolio.forced_exits) if compounded.portfolio else 0.0,
    }

    finding = Finding(
        name="delisting",
        honest=detail["mean_label_compound"],
        pathological=detail["mean_label_drop"],
        units="mean forward return in the study",
        note=(f"{departures} names taken to -100%; dropping their truncated labels removes "
              f"{lost_rows} rows carrying {lost_return:+.1f} of cumulative return from the "
              f"study"),
    )
    return finding, detail


# ---------------------------------------------------------------------------
# 4. What assuming a zero delisting return is worth
# ---------------------------------------------------------------------------


FLATTERY_SPEC = PanelSpec(
    n_securities=150,
    n_periods=1000,
    lam=0.0006,
    seed=20260921,
    delist_hazard=0.0015,
    delist_exposure_tilt=-0.8,
)
"""Failures among the names the book is **long**, which is when the assumption
flatters.

Same lesson as survivorship, and it has to be stated again because it is the
thing everybody gets wrong: the sign follows which leg the failures were in. A
long-short book that is long the names that fail is flattered by ignoring what
failure cost; one that is short them is understated. Both are measured."""


def measure_delisting_flattery(spec: PanelSpec = FLATTERY_SPEC
                               ) -> tuple[Finding, dict[str, float]]:
    """The same strategy, both delisting models, on one panel.

    Session D Part 2.3. `last_price` liquidates at the final quoted price and
    applies no delisting return -- the default, and what a backtest does when
    nobody has asked the question. `apply_true_return` uses what the generator
    knows actually happened. The difference is what the assumption is worth.
    """
    results: dict[str, dict[str, float]] = {}
    for tilt, label in ((spec.delist_exposure_tilt, "long_the_failures"),
                        (-spec.delist_exposure_tilt, "short_the_failures")):
        panel = generate_panel(spec.replace(delist_exposure_tilt=tilt))
        for model in (LAST_PRICE, APPLY_TRUE_RETURN):
            result = run_pipeline(panel, PipelineConfig(delisting_model=model))
            returns = result.portfolio.active_returns
            results[f"{label}/{model}"] = {
                "total_return": float((1.0 + returns).prod() - 1.0),
                "sharpe": result.sharpe,
                "delisting_pnl": result.portfolio.delisting_loss,
                "departures": float(panel.frame["delisted"].sum()),
                "distress_share": float(
                    (panel.delistings["cause"] == "distress").mean())
                if len(panel.delistings) else float("nan"),
                "worst_delisting": float(panel.delistings["delisting_return"].min())
                if len(panel.delistings) else float("nan"),
            }

    assumed = results["long_the_failures/last_price"]
    truth = results["long_the_failures/apply_true_return"]
    detail = {f"{key}__{stat}": value
              for key, row in results.items() for stat, value in row.items()}
    detail["flattery_return"] = assumed["total_return"] - truth["total_return"]
    detail["flattery_sharpe"] = assumed["sharpe"] - truth["sharpe"]
    detail["mirror_return"] = (results["short_the_failures/last_price"]["total_return"]
                               - results["short_the_failures/apply_true_return"]["total_return"])
    detail["mirror_sharpe"] = (results["short_the_failures/last_price"]["sharpe"]
                               - results["short_the_failures/apply_true_return"]["sharpe"])

    return Finding(
        name="assumed exit",
        honest=truth["total_return"],
        pathological=assumed["total_return"],
        units="total return over the panel",
        note=(f"{int(assumed['departures'])} departures, "
              f"{assumed['distress_share']:.0%} distressed, worst "
              f"{assumed['worst_delisting']:.0%}; Sharpe "
              f"{truth['sharpe']:+.2f} -> {assumed['sharpe']:+.2f}"),
    ), detail


# ---------------------------------------------------------------------------
# Running them
# ---------------------------------------------------------------------------


def run_all(verbose: bool = True) -> tuple[dict[str, Finding], dict[str, dict[str, float]]]:
    """Every measurement in this milestone. Slow: it generates several panels."""
    findings: dict[str, Finding] = {}
    details: dict[str, dict[str, float]] = {}

    for name, measure in (("survivorship", measure_survivorship),
                          ("lookahead", measure_lookahead),
                          ("delisting", measure_delisting),
                          ("assumed_exit", measure_delisting_flattery)):
        if verbose:
            print(f"{name} ...", flush=True)
        finding, detail = measure()
        findings[name] = finding
        details[name] = detail

    return findings, details


def main(argv: list[str] | None = None) -> int:
    # Imported here rather than at module scope: the report imports this module
    # for its constants, and two modules importing each other at import time is a
    # cycle that works until somebody imports them in the other order.
    from research.pathology_report import REPORT, write_report

    parser = argparse.ArgumentParser(description="M7: pathology injection")
    parser.add_argument("--out", type=Path, default=REPORT)
    args = parser.parse_args(argv)

    findings, details = run_all()
    print()
    for finding in findings.values():
        print(f"  {finding}")
    print()
    print(f"wrote {write_report(findings, details, args.out)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


