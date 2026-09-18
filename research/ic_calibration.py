"""M8b: an information coefficient bar measured instead of assumed.

    python -m research.ic_calibration            # the committed run
    python -m research.ic_calibration --quick    # the machinery, in a minute

M8 measured that the IC's t-statistic is not calibrated: on data with no signal
it rejects at 16.6% against a nominal 5%, and the rate climbs with the horizon --
8.9% at one period, 17.3% at five, 23.6% at twenty. The cause is not mysterious.
Consecutive information coefficients at a horizon of h are built from returns
that share h-1 periods, `PipelineResult.ic_t` corrects for that with a Newey-West
standard error at h-1 lags, and that correction is well known to be
anti-conservative in finite samples: at twenty lags the Bartlett estimator is
itself noisy, and it under-corrects.

WHY THIS DOES NOT TRY TO FIX THE ESTIMATOR

Prewhitening, automatic bandwidth selection, a different kernel -- each is a
plausible improvement and none of them comes with a guarantee at this sample
size. Chasing an analytical correction would produce a number nobody could
check.

So the IC is calibrated the way the Sharpe bar was: point the null at it and
measure what it does. The harness already exists, the panel is already known to
be empty, and the output is a threshold in the units the statistic is reported
in rather than a t-value that means something different at every horizon.

WHAT COMES OUT, PER HORIZON

| the nominal rule | the |IC| a t-statistic of 1.96 implicitly demands |
| its measured cost | how often that rule fires on nothing |
| single-test bar | the |IC| the null exceeds 5% of the time |
| best-of-N bar | the same, for the best of a search |

The single-test bar and the nominal rule are directly comparable -- both are an
|IC| -- which is the point. A reader should see the gap without doing arithmetic.

ABSOLUTE VALUES, THROUGHOUT

A researcher who finds a feature with an IC of -0.05 negates the feature. So the
statistic a search maximizes is |IC|, and every threshold here is on |IC|. The
sign-flip doubles the effective number of tries, and the effective-tries
calibration absorbs that automatically: it solves for whatever try count matches
the observed best, without being told where the count came from.
"""

from __future__ import annotations

import argparse
import itertools
import json
from pathlib import Path

import numpy as np
import pandas as pd

from research.panel import PanelSpec
from research.pipeline import PipelineConfig
from research.sweep import (
    CONVENTIONAL_T,
    DEFAULT_PANELS,
    NULL_SPEC,
    _FEATURE_SETS,
    effective_tries,
    max_quantile,
    run_sweep,
)

REPO = Path(__file__).resolve().parent.parent
RESULTS = REPO / "docs" / "figures" / "ic_calibration.json"
FIGURE = REPO / "docs" / "figures" / "ic_calibration.png"

IC_HORIZONS = (1, 2, 5, 10, 20, 40)
"""Every horizon this panel can carry, and both of the ones M8 already measured.

Forty is the longest that leaves a purged five-fold split with training data on
a 750-period panel; beyond it the folds start starving and the threshold would be
measuring the fold structure rather than the statistic.
"""

_MODELS: tuple[tuple[str, dict[str, float | int]], ...] = (
    ("ridge", {"alpha": 1.0}),
    ("gbm", {"max_depth": 3}),
)
"""Two, not four. The IC's miscalibration is a property of the autocorrelation in
a series of per-date rank correlations, which is about the horizon and the
persistence of the features -- not about which model produced the scores. Two
model families are enough to show that, and halving the family buys the horizons
this milestone actually needs."""

_SURFACE = "#fcfcfb"
_INK = "#0b0b0b"
_MUTED = "#52514e"
_SERIES = "#2a78d6"
_WARN = "#eb6834"
_GRID = "#d9d8d4"


def ic_variants(horizons: tuple[int, ...] = IC_HORIZONS) -> list[PipelineConfig]:
    """The variant family, without the portfolio stage.

    No book is built: the information coefficient does not need one, and the
    portfolio layer is most of the cost of a run. Six horizons for the price of
    two.
    """
    out: list[PipelineConfig] = []
    for (name, features), horizon, (model, kwargs) in itertools.product(
            _FEATURE_SETS, horizons, _MODELS):
        suffix = "".join(f",{key}={value:g}" for key, value in sorted(kwargs.items()))
        out.append(PipelineConfig(
            features=features,
            horizon=horizon,
            model=model,
            model_kwargs=dict(kwargs),
            embargo=max(horizon, 5),
            build_portfolio=False,
            label=f"{name}|h{horizon}|{model}{suffix}",
        ))
    return out


def run_ic_sweep(panels: int = DEFAULT_PANELS, spec: PanelSpec = NULL_SPEC,
                 horizons: tuple[int, ...] = IC_HORIZONS,
                 verbose: bool = True) -> pd.DataFrame:
    """The null sweep, across horizons. Same panels, same generator as M8."""
    return run_sweep(panels=panels, spec=spec, configs=ic_variants(horizons), verbose=verbose)


def ic_thresholds(frame: pd.DataFrame, percentile: float = 0.95) -> pd.DataFrame:
    """One row per horizon: what the nominal rule costs, and what replaces it."""
    rows = []
    for horizon, group in frame.groupby("horizon", sort=True):
        ic = group["ic_mean"].dropna()
        if ic.empty:
            continue
        magnitude = ic.abs()
        sigma = float(ic.std(ddof=1))

        # The |IC| a t-statistic of 1.96 implicitly demands. t = ic / se, so
        # se = ic / t, and the rule fires at 1.96 * se. Taken as a median across
        # runs because a run whose t landed near zero has a meaningless ratio.
        usable = group.loc[group["ic_t"].abs() > 0.25]
        implied = (CONVENTIONAL_T * (usable["ic_mean"] / usable["ic_t"]).abs()).median()

        per_panel_best = group.groupby("panel")["ic_mean"].apply(lambda s: s.abs().max())
        variants = int(group["variant"].nunique())
        mean_best = float(per_panel_best.mean())
        # Calibrated against *twice* the variant count, because the statistic being
        # maximized is |IC|: by symmetry the largest of n absolute draws behaves
        # like the largest of 2n signed ones, since a researcher who finds a
        # negative IC negates the feature. Searching only up to n pinned the
        # calibration at the ceiling on four of six horizons, which is the search
        # hitting its bound rather than an answer.
        n_effective = effective_tries(sigma, mean_best, 2 * variants)

        rows.append({
            "horizon": int(horizon),
            "runs": int(len(ic)),
            "variants": variants,
            "ic_sd": sigma,
            "nominal_ic": float(implied),
            "t_false_positive": float((group["ic_t"].abs() > CONVENTIONAL_T).mean()),
            "single_test": float(np.quantile(magnitude, percentile)),
            "mean_best": mean_best,
            "n_effective": float(n_effective),
            "best_of_n": max_quantile(sigma, n_effective, percentile),
        })
    return pd.DataFrame(rows)


def summarize(frame: pd.DataFrame) -> dict[str, object]:
    table = ic_thresholds(frame)
    return {
        "runs": int(len(frame)),
        "panels": int(frame["panel"].nunique()),
        "horizons": [int(h) for h in table["horizon"]],
        "variants_per_horizon": int(table["variants"].iloc[0]) if len(table) else 0,
        "by_horizon": table.to_dict(orient="records"),
    }


def plot_ic_calibration(summary: dict[str, object], path: Path = FIGURE) -> Path:
    """The nominal rule against the measured one, per horizon."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    table = pd.DataFrame(summary["by_horizon"])
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)

    figure, (top, bottom) = plt.subplots(2, 1, figsize=(7.2, 6.4), dpi=200, sharex=True,
                                         gridspec_kw={"height_ratios": [2, 1]})
    figure.patch.set_facecolor(_SURFACE)
    for axes in (top, bottom):
        axes.set_facecolor(_SURFACE)
        axes.grid(True, color=_GRID, linewidth=0.8, alpha=0.9)
        axes.set_axisbelow(True)
        for side in ("top", "right"):
            axes.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            axes.spines[side].set_color(_GRID)
        axes.tick_params(colors=_MUTED, labelsize=9)

    horizons = table["horizon"]
    top.plot(horizons, table["nominal_ic"], marker="o", markersize=7, linewidth=2,
             color=_MUTED, linestyle=(0, (4, 3)), label="what a t-statistic of 1.96 demands")
    top.plot(horizons, table["single_test"], marker="o", markersize=7, linewidth=2,
             color=_SERIES, label="measured single-test bar")
    top.plot(horizons, table["best_of_n"], marker="o", markersize=7, linewidth=2,
             color=_WARN, label=f"measured best-of-{summary['variants_per_horizon']} bar")
    top.set_ylabel("|information coefficient|", color=_MUTED, fontsize=10)
    top.set_title("The information coefficient bar, measured against this pipeline's null",
                  color=_INK, fontsize=13, pad=26, loc="left")
    top.text(0.0, 1.03, f"{int(summary['runs'])} runs over {int(summary['panels'])} panels "
                        f"with lambda = 0",
             transform=top.transAxes, color=_MUTED, fontsize=9.5)
    legend = top.legend(loc="upper left", frameon=False, fontsize=9)
    for text in legend.get_texts():
        text.set_color(_MUTED)

    bottom.axhline(0.05, color=_MUTED, linewidth=1.5, linestyle=(0, (4, 3)))
    bottom.plot(horizons, table["t_false_positive"], marker="o", markersize=7, linewidth=2,
                color=_WARN)
    bottom.set_ylabel("false positives", color=_MUTED, fontsize=10)
    bottom.set_xlabel("label horizon (periods)", color=_MUTED, fontsize=10)
    bottom.set_ylim(0.0, max(0.3, float(table["t_false_positive"].max()) * 1.2))
    bottom.yaxis.set_major_formatter(lambda value, _: f"{value:.0%}")
    bottom.text(0.99, 0.08, "nominal 5%", transform=bottom.transAxes, color=_MUTED,
                fontsize=9, ha="right")
    bottom.text(0.0, 1.06, "how often the t-statistic rule fires when nothing is there",
                transform=bottom.transAxes, color=_MUTED, fontsize=9.5)

    figure.tight_layout()
    figure.savefig(path, facecolor=_SURFACE)
    plt.close(figure)
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="M8b: empirical IC thresholds")
    parser.add_argument("--panels", type=int, default=DEFAULT_PANELS)
    parser.add_argument("--out", type=Path, default=RESULTS)
    parser.add_argument("--quick", action="store_true",
                        help="two horizons on two small panels; checks the machinery")
    args = parser.parse_args(argv)

    spec = NULL_SPEC
    horizons = IC_HORIZONS
    panels = args.panels
    configs = None
    if args.quick:
        horizons = (1, 20)
        spec = spec.replace(n_securities=60, n_periods=400)
        panels = 2
        configs = [config for config in ic_variants(horizons)
                   if config.features[0].source == "exposure"][:8]

    configs = configs or ic_variants(horizons)
    print(f"IC calibration: {len(configs)} variants x {panels} panels = "
          f"{len(configs) * panels} runs")
    frame = run_sweep(panels=panels, spec=spec, configs=configs)
    summary = summarize(frame)

    table = pd.DataFrame(summary["by_horizon"])
    print()
    print(table.to_string(index=False, formatters={
        "ic_sd": "{:.4f}".format,
        "nominal_ic": "{:.4f}".format,
        "t_false_positive": "{:.1%}".format,
        "single_test": "{:.4f}".format,
        "mean_best": "{:.4f}".format,
        "n_effective": "{:.0f}".format,
        "best_of_n": "{:.4f}".format,
    }))

    if not args.quick:
        path = Path(args.out)
        path.parent.mkdir(parents=True, exist_ok=True)
        kept = ["panel", "seed", "variant", "model", "horizon", "ic_mean", "ic_t",
                "lambda_t", "oos_r2", "n_rows"]
        path.write_text(json.dumps({
            "spec": spec.to_dict(),
            "summary": summary,
            "runs": frame[kept].round(6).to_dict(orient="records"),
        }, indent=2, default=str), encoding="utf-8")
        print(f"\nwrote {path}")
        print(f"wrote {plot_ic_calibration(summary)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
