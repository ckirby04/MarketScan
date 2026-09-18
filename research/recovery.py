"""M6: does the pipeline find the lam that was planted?

    python -m research.recovery                       # the committed figure
    python -m research.recovery --quick               # a fast sanity pass

Everything before this milestone is machinery. This is the first measurement, and
it is the one that licenses the rest: for a sweep of lam values -- including ones
small enough to be realistic -- generate a panel, run the whole pipeline, and ask
whether the estimate's error bars contain the truth.

WHY A PLOT AND NOT A NUMBER

A single lam recovered correctly proves almost nothing: any estimator with the
right units gets one point right. What matters is the *shape* across lam -- that
the estimate tracks the truth with a slope of one and does not drift, flatten at
the small end, or scale. The plot shows that in a way a table of six numbers does
not, which is why the definition of done asks for one.

The panels here have **no churn**. A departure hazard tilted by the exposure makes
the exposure predict two things -- a slightly different return, and a -30% exit --
and the total relationship is then genuinely larger than lam. Recovering lam on
such a panel would require one bias to cancel another. That extra channel is M7's
subject, measured there rather than smuggled in here.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path

import pandas as pd

from research.panel import PanelSpec, generate_panel
from research.pipeline import PipelineConfig, PipelineResult, run_pipeline

REPO = Path(__file__).resolve().parent.parent
FIGURE = REPO / "docs" / "figures" / "lambda_recovery.png"
DATA = REPO / "docs" / "figures" / "lambda_recovery.json"

DEFAULT_LAMS = (0.0, 0.0001, 0.00025, 0.0005, 0.001, 0.002)
"""Including zero and including small ones. A recovery test that only sweeps
large values is a test of arithmetic, not of an estimator: the interesting
question is where the signal disappears into the noise, and that is down here."""

DEFAULT_SEEDS = (20260918, 20260919, 20260920)

# From the project's reference palette: one categorical hue for the measurement,
# neutral ink for everything that is not data. A reference line is not a series
# and does not get a hue.
_SURFACE = "#fcfcfb"
_INK = "#0b0b0b"
_MUTED = "#52514e"
_SERIES = "#2a78d6"
_GRID = "#d9d8d4"


@dataclass(frozen=True)
class RecoveryPoint:
    """One (lam, seed) experiment."""

    lam: float
    seed: int
    estimate: float
    stderr: float
    covers: bool
    ic_mean: float
    ic_t: float
    oos_r2: float
    n_periods: int

    @classmethod
    def of(cls, result: PipelineResult, seed: int) -> "RecoveryPoint":
        return cls(
            lam=result.panel_lam,
            seed=seed,
            estimate=result.lambda_hat.estimate,
            stderr=result.lambda_hat.stderr,
            covers=result.recovers(sigmas=2.0),
            ic_mean=result.ic_mean,
            ic_t=result.ic_t,
            oos_r2=result.oos_r2,
            n_periods=result.lambda_hat.n_periods,
        )


def recovery_sweep(lams: tuple[float, ...] = DEFAULT_LAMS,
                   seeds: tuple[int, ...] = DEFAULT_SEEDS,
                   spec: PanelSpec | None = None,
                   config: PipelineConfig | None = None,
                   verbose: bool = False) -> pd.DataFrame:
    """Runs the pipeline once per (lam, seed). Returns one row per run."""
    base = spec or PanelSpec(n_securities=200, n_periods=1500)
    if base.delist_hazard:
        raise ValueError("recovery runs on panels without churn; see the module docstring")
    settings = config or PipelineConfig(build_portfolio=False)

    rows: list[RecoveryPoint] = []
    for lam in lams:
        for seed in seeds:
            panel = generate_panel(base.replace(lam=lam, seed=seed))
            result = run_pipeline(panel, settings)
            point = RecoveryPoint.of(result, seed)
            rows.append(point)
            if verbose:
                print(f"  lam={lam:<8g} seed={seed}  "
                      f"lam_hat={point.estimate:+.6f} +/- {point.stderr:.6f}  "
                      f"{'covers' if point.covers else 'MISSES'}")
    return pd.DataFrame([vars(row) for row in rows])


def aggregate(frame: pd.DataFrame) -> pd.DataFrame:
    """Per lam: the mean estimate, and the error bar to draw around it.

    The bar is the mean of the per-run standard errors rather than the spread of
    the estimates across seeds. Both are defensible; this one is the honest choice
    because it is the interval a single study would have reported, and the
    question this figure answers is whether *a* study recovers lam -- not whether
    the average of three does.
    """
    grouped = frame.groupby("lam")
    return pd.DataFrame({
        "estimate": grouped["estimate"].mean(),
        "stderr": grouped["stderr"].mean(),
        "spread": grouped["estimate"].std(ddof=1),
        "covered": grouped["covers"].sum(),
        "runs": grouped["covers"].size(),
        "ic_mean": grouped["ic_mean"].mean(),
        "oos_r2": grouped["oos_r2"].mean(),
    }).reset_index()


def plot_recovery(frame: pd.DataFrame, path: Path = FIGURE) -> Path:
    """Estimated against true lam, with error bars and the line y = x."""
    import matplotlib
    matplotlib.use("Agg")  # no display in CI, and none wanted
    import matplotlib.pyplot as plt

    summary = aggregate(frame)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)

    scale = 1e4  # basis points per period: readable axes instead of 0.00025
    figure, axes = plt.subplots(figsize=(7.2, 5.4), dpi=200)
    figure.patch.set_facecolor(_SURFACE)
    axes.set_facecolor(_SURFACE)

    limit = float(max(summary["lam"].max(), summary["estimate"].max()) * scale) * 1.15
    floor = min(0.0, float(summary["estimate"].min() * scale) * 1.3)
    axes.plot([floor, limit], [floor, limit], linewidth=1.5, linestyle=(0, (4, 3)),
              color=_MUTED, label="perfect recovery", zorder=1)

    # Individual runs, faint: the same quantity as the mean, not a second series,
    # so the same hue at lower opacity rather than a second colour.
    axes.scatter(frame["lam"] * scale, frame["estimate"] * scale, s=18, color=_SERIES,
                 alpha=0.35, linewidths=0, zorder=2)

    axes.errorbar(summary["lam"] * scale, summary["estimate"] * scale,
                  yerr=2.0 * summary["stderr"] * scale,
                  fmt="o", markersize=8, linewidth=0, elinewidth=1.6, capsize=5,
                  color=_SERIES, markeredgecolor=_SURFACE, markeredgewidth=1.2,
                  label="estimated lambda (+/- 2 s.e.)", zorder=3)

    axes.set_xlabel("planted lambda  (basis points per period, per unit exposure)",
                    color=_MUTED, fontsize=10)
    axes.set_ylabel("estimated lambda  (bp per period)", color=_MUTED, fontsize=10)
    axes.set_title("The pipeline recovers the signal it was given",
                   color=_INK, fontsize=13, pad=26, loc="left")

    runs = int(summary["runs"].sum())
    covered = int(summary["covered"].sum())
    subtitle = (f"{runs} runs, {len(summary)} values of lambda, "
                f"{covered}/{runs} within two standard errors of the truth")
    axes.text(0.0, 1.022, subtitle, transform=axes.transAxes, color=_MUTED, fontsize=9.5)

    axes.grid(True, color=_GRID, linewidth=0.8, alpha=0.9)
    axes.set_axisbelow(True)
    for side in ("top", "right"):
        axes.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        axes.spines[side].set_color(_GRID)
    axes.tick_params(colors=_MUTED, labelsize=9)
    axes.set_xlim(floor, limit)
    axes.set_ylim(floor, limit)

    legend = axes.legend(loc="upper left", frameon=False, fontsize=9.5)
    for text in legend.get_texts():
        text.set_color(_MUTED)

    figure.tight_layout()
    figure.savefig(path, facecolor=_SURFACE)
    plt.close(figure)
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="M6: lambda recovery")
    parser.add_argument("--out", type=Path, default=FIGURE)
    parser.add_argument("--securities", type=int, default=200)
    parser.add_argument("--periods", type=int, default=1500)
    parser.add_argument("--horizon", type=int, default=1)
    parser.add_argument("--quick", action="store_true",
                        help="a smaller sweep, for checking the machinery rather than "
                             "producing the committed figure")
    args = parser.parse_args(argv)

    spec = PanelSpec(n_securities=args.securities,
                     n_periods=400 if args.quick else args.periods)
    seeds = DEFAULT_SEEDS[:1] if args.quick else DEFAULT_SEEDS

    print(f"recovery sweep: {len(DEFAULT_LAMS)} lambdas x {len(seeds)} seeds, "
          f"{spec.n_securities} names x {spec.n_periods} periods")
    frame = recovery_sweep(lams=DEFAULT_LAMS, seeds=seeds, spec=spec,
                           config=PipelineConfig(horizon=args.horizon, build_portfolio=False),
                           verbose=True)

    summary = aggregate(frame)
    print()
    print(summary.to_string(index=False,
                            formatters={"lam": "{:.6f}".format,
                                        "estimate": "{:+.6f}".format,
                                        "stderr": "{:.6f}".format,
                                        "spread": "{:.6f}".format,
                                        "ic_mean": "{:+.4f}".format,
                                        "oos_r2": "{:+.5f}".format}))

    covered = int(summary["covered"].sum())
    runs = int(summary["runs"].sum())
    print(f"\n{covered}/{runs} runs recovered lambda within two standard errors")

    path = plot_recovery(frame, args.out)
    print(f"wrote {path}")

    if not args.quick:
        DATA.parent.mkdir(parents=True, exist_ok=True)
        DATA.write_text(json.dumps({
            "spec": spec.to_dict(),
            "lams": list(DEFAULT_LAMS),
            "seeds": list(seeds),
            "runs": frame.to_dict(orient="records"),
        }, indent=2, default=str), encoding="utf-8")
        print(f"wrote {DATA}")

    # A recovery sweep that misses is a result, not a crash: the exit code says
    # so so that a caller in CI does not have to parse the table.
    return 0 if covered == runs else 1


if __name__ == "__main__":
    raise SystemExit(main())
