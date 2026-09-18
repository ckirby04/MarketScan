"""Body of the M8 report. Imported by `sweep_report.write_report`.

Split out only because the report is long; `sweep_report.py` keeps the entry
point and the verdict bands, and this holds the prose that surrounds the tables.
"""

from __future__ import annotations

import pandas as pd

from research.sweep import CONVENTIONAL_T, NULL_SPEC, STRICT_T


def experiment(summary: dict[str, object], variants: int) -> list[str]:
    return [
        "## The experiment",
        "",
        f"**{int(summary['panels'])} independently seeded synthetic panels**, each",
        f"{NULL_SPEC.n_securities} names over {NULL_SPEC.n_periods} business days, with",
        "**lambda = 0**: returns are a market factor plus noise, and the exposure column is",
        "an AR(1) process wired to nothing. Names still enter and leave, so the portfolio",
        "layer still has forced exits to handle -- but the departure hazard is *untilted*,",
        "because a hazard that depends on the exposure makes the exposure predict returns",
        "through the departure channel and puts a real signal into a panel that has to be",
        "empty. The first version of this sweep had exactly that, and it was re-run. A test",
        "pins it now.",
        "",
        f"Against each panel, **{variants} pipeline variants** -- 21 feature sets, three",
        f"horizons, four model configurations -- for **{int(summary['runs'])} runs** in",
        "total. Each produces an information coefficient, an estimated lambda, an",
        "out-of-sample R-squared, and a frictionless Sharpe from a constrained long-short",
        "book.",
        "",
        "Nothing in any of it is real. Every number below is the pipeline measuring itself.",
        "",
    ]


def single_variant(summary: dict[str, object]) -> list[str]:
    conventional = summary["fdr_conventional"]
    strict = summary["fdr_strict"]
    by_horizon = summary["fdr_by_horizon"]

    lines = [
        "## What a single variant finds",
        "",
        "| | |",
        "|---|---|",
        f"| mean Sharpe across all runs | {summary['sharpe_mean']:+.3f} |",
        f"| standard deviation | {summary['sharpe_sd']:.3f} |",
        f"| range | {summary['sharpe_min']:+.2f} to {summary['sharpe_max']:+.2f} |",
        f"| skew / excess kurtosis | {summary['sharpe_skew']:+.2f} / "
        f"{summary['sharpe_kurtosis']:+.2f} |",
        "",
        "The mean is zero to three decimal places, which is the first thing to check: a",
        "null panel that produced a positive average Sharpe would be a null panel with",
        "something in it. The distribution is close enough to normal to build on, and the",
        "threshold below does.",
        "",
        "### False discovery rate, one variant at a time",
        "",
        "The fraction of runs that cleared a conventional significance bar. On data with no",
        f"signal a calibrated two-sided test rejects 5% of the time at |t| > {CONVENTIONAL_T},",
        "so this measures whether the machinery delivers the rate it advertises.",
        "",
        "| statistic | " + f"\\|t\\| > {CONVENTIONAL_T:.2f}" + " | "
        + f"\\|t\\| > {STRICT_T:.2f}" + " | trials |",
        "|---|---|---|---|",
        f"| information coefficient | **{conventional['ic_t']:.1%}** | {strict['ic_t']:.1%} | "
        f"{int(conventional['ic_t_n'])} |",
        f"| portfolio Sharpe | {conventional['sharpe_t']:.1%} | {strict['sharpe_t']:.1%} | "
        f"{int(conventional['sharpe_t_n'])} |",
        f"| estimated lambda | {conventional['lambda_t']:.1%} | {strict['lambda_t']:.1%} | "
        f"{int(conventional['lambda_t_n'])} |",
        "",
        "**The Sharpe is calibrated. The information coefficient is not.**",
        "",
        f"At {conventional['sharpe_t']:.1%}, the Sharpe's t-statistic rejects at about the",
        f"rate it promises. The information coefficient rejects at "
        f"{conventional['ic_t']:.1%} -- more than three times nominal -- and the reason is",
        "visible in the breakdown by horizon:",
        "",
        "| horizon | IC | Sharpe |",
        "|---|---|---|",
    ]
    for horizon in sorted(by_horizon):
        row = by_horizon[horizon]
        lines.append(f"| {horizon} | {row['ic_t']:.1%} | {row['sharpe_t']:.1%} |")

    lines += [
        "",
        "The IC's false-positive rate **rises with the horizon**, from",
        f"{by_horizon[min(by_horizon)]['ic_t']:.1%} at one period to",
        f"{by_horizon[max(by_horizon)]['ic_t']:.1%} at twenty. Consecutive information",
        "coefficients at a horizon of h are computed from returns that share h-1 periods,",
        "and `PipelineResult.ic_t` corrects for that with a Newey-West standard error at",
        "h-1 lags. The correction is in the right direction and is **not enough**: at",
        "twenty lags the Bartlett estimator is itself noisy, and it under-corrects.",
        "",
        "The practical consequence, stated as a rule: **do not use the IC t-statistic in",
        "this codebase as an inferential statistic.** It is a fine descriptive measure of",
        "how well a score ranks a cross-section. As a significance test at a long horizon",
        "it is wrong by a factor of four, and it is wrong in the flattering direction.",
        "",
        "The lambda row is a different kind of number and its trials column says so. That",
        "estimate is measured against the panel's raw exposure, so it does not depend on a",
        "variant's features or model at all -- only on the panel, the horizon, and how far",
        f"the feature windows warmed up. {int(conventional['ic_t_n'])} runs carry",
        f"{int(conventional['lambda_t_n'])} distinct estimates, and those are themselves",
        "highly correlated: six warm-up lengths per (panel, horizon) differing only in",
        "where the sample starts. Read the denominator as an upper bound on the trials.",
        "",
        f"On it, the Fama-MacBeth estimator rejects at {conventional['lambda_t']:.1%}. That",
        "is the estimator behaving -- and it is the one M6 measures lambda with, which is",
        "the reassuring half of this table.",
        "",
    ]
    return lines


def the_search(summary: dict[str, object], variants: int) -> list[str]:
    best = pd.DataFrame(summary["best_of_n"])
    maxima = ", ".join(f"{value:+.2f}" for value in summary["panel_maxima"])

    lines = [
        "## What a *search* finds",
        "",
        "Nobody reports the average of the variants they tried. They report the best one.",
        "So the distribution that matters is of the maximum.",
        "",
        f"The six panels give six realizations of best-of-{variants}: {maxima} -- mean",
        f"**{summary['mean_best']:+.2f}**, standard deviation {summary['sd_best']:.2f}.",
        "",
        "| variants tried | mean best | median | 90th | 95th | 99th |",
        "|---|---|---|---|---|---|",
    ]
    for row in best.itertuples():
        lines.append(f"| {int(row.n)} | {row.mean:+.2f} | {row.median:+.2f} | "
                     f"{row.p90:+.2f} | {row.p95:+.2f} | {row.p99:+.2f} |")

    lines += [
        "",
        "That table is produced by resampling variants within a panel, and **its upper",
        "percentiles at large N are a floor, not an estimate.** Drawing 252 variants from a",
        "panel that has 252 returns the panel maximum every time, so the 90th, 95th and",
        f"99th percentiles all collapse onto {summary['resampled_p95_floor']:+.2f} -- the",
        "largest Sharpe anywhere in the sweep. Six panels cannot resolve the tail of a",
        "best-of-252 distribution, and pretending otherwise would be the same error this",
        "whole document is about.",
        "",
        "### So the threshold is built from what the sweep does estimate",
        "",
        f"1. The per-variant null Sharpe distribution, from all {int(summary['runs'])} runs:",
        f"   mean {summary['sharpe_mean']:+.3f}, standard deviation "
        f"{summary['sharpe_sd']:.3f}, close to normal.",
        "2. The maximum of *n* draws from a normal is Gumbel, with a location and scale",
        "   that follow from the tail.",
        f"3. The observed mean best-of-{variants} of {summary['mean_best']:+.2f} calibrates",
        f"   *n*: **{variants} correlated variants behave like",
        f"   {summary['n_effective']:.0f} independent ones.** Variants sharing a feature or",
        "   a horizon are not separate tries, and assuming they were would set the bar too",
        "   high -- a different way of being wrong, not a safer one.",
        "4. The threshold is the 95th percentile of that calibrated distribution.",
        "",
        "| | |",
        "|---|---|",
        f"| expected best-of-{variants}, independent | "
        f"{summary['expected_max_analytic']:+.2f} |",
        f"| observed mean best-of-{variants} | {summary['mean_best']:+.2f} |",
        f"| effective independent tries | {summary['n_effective']:.0f} |",
        f"| 95th percentile, calibrated | **{summary['threshold']:+.2f}** |",
        f"| 95th percentile, assuming independence | {summary['threshold_independent']:+.2f} |",
        "",
        "The observed mean best sits *below* what independence predicts, which is the",
        "expected direction and a check that step 3 is doing something real: correlated",
        "tries explore less.",
        "",
    ]
    return lines


def champion(frame: pd.DataFrame) -> list[str]:
    best = frame.loc[frame["sharpe"].idxmax()]
    return [
        "### The single best variant of the whole sweep",
        "",
        "| | |",
        "|---|---|",
        f"| configuration | `{best['variant']}` |",
        f"| Sharpe | **{best['sharpe']:+.2f}** |",
        f"| information coefficient | {best['ic_mean']:+.4f} (t = {best['ic_t']:+.2f}) |",
        f"| out-of-sample R-squared | {best['oos_r2']:+.5f} |",
        "",
        "A specific, reasonable-looking configuration, on a panel with no signal in it at",
        "all.",
        "",
        f"Note what its own statistics say: an information coefficient of {best['ic_mean']:+.4f}",
        f"at t = {best['ic_t']:+.2f}, and an out-of-sample R-squared of {best['oos_r2']:+.5f} --",
        "**negative**, meaning its forecasts were worse than predicting zero. The Sharpe is",
        "the only flattering number it has, and the Sharpe is the number that would have",
        "been reported. Nothing about the way it was found differs from the way a real one",
        "would be found.",
        "",
    ]


def caveats(summary: dict[str, object], variants: int) -> list[str]:
    return [
        "## What this number is not",
        "",
        "- **It is not transferable to a different search.** The threshold is a function of",
        f"  how many variants were tried ({variants}, behaving like",
        f"  {summary['n_effective']:.0f}), the length of the sample (three years), and how",
        "  correlated the variants are. More variants raise it; a longer sample lowers it.",
        "- **It is frictionless.** The research layer charges no commission and crosses no",
        "  spread, by design -- the C++ engine owns execution. Costs would lower the Sharpes",
        "  being judged as well as the bar, and not by the same amount.",
        "- **It is synthetic and Gaussian.** The null is a market factor plus normal noise.",
        "  Real returns have fat tails, volatility clustering and cross-sectional structure",
        "  this panel does not contain, all of which give a search more to find. Treat the",
        "  threshold as a floor.",
        f"- **It rests on {int(summary['panels'])} panels.** The per-variant distribution is",
        "  well estimated from 1512 runs; the calibration of effective tries rests on six",
        "  observations of the maximum. More panels would tighten it, and that is the",
        "  single highest-value thing to do to this measurement.",
        "",
        "## Why this is the milestone that matters",
        "",
        "Everything before it establishes that the instrument responds to a signal. This",
        "establishes what it does when there is none -- and that is the number a result has",
        "to be compared against before it means anything. A pipeline that has not measured",
        "its own false discovery rate is not a measuring instrument. It is a search, and a",
        "search always finds something.",
        "",
    ]
