"""M8: how often does this pipeline find something in data containing nothing?

    python -m research.sweep                 # the committed run
    python -m research.sweep --quick         # a few variants, to check the machinery

**This is the milestone that matters.** Every result this project ever produces is
worth exactly as much as this number. A pipeline that reports a significant
finding one time in three on data with no signal in it has not measured anything;
it has measured itself.

THE EXPERIMENT

Set lambda to zero. Nothing in the panel predicts anything -- returns are a market
factor and noise, and the exposure column is an AR(1) process wired to nothing.
Then run a large family of pipeline variants over it: different features,
different horizons, different models, different hyperparameters, the way an
analyst does over a fortnight, and count how many come back significant at
conventional thresholds.

Two numbers come out, and they answer different questions:

**The false discovery rate** is what fraction of individual variants clear
t > 1.96. If the tests were perfectly calibrated this would be 5% by
construction, so what it actually measures is whether the machinery -- overlapping
labels, autocorrelated features, a Newey-West correction that may or may not be
enough -- delivers the nominal rate.

**The best-of-N Sharpe** is the number that changes behaviour. Nobody reports the
average of the variants they tried. They report the best one. So the question a
result has to answer is not "is this Sharpe better than zero" but "is this Sharpe
better than the best of N tries on nothing", and the answer is a threshold that
grows with how many things were tried.

WHERE THE THRESHOLD COMES FROM

Each null panel yields one Sharpe per variant, so a sweep of P panels yields P
observations of "the best of N tries" -- six, here. Six is not a distribution, and
the first version of this module tried to get one by resampling N variants from
within a panel and taking the best. That does not work and the numbers say so:
drawing 252 variants from a panel that has exactly 252 returns the panel maximum
every time, so the resampled 90th, 95th and 99th percentiles are all the single
largest Sharpe in the sweep. A floor, reported as a percentile.

So the threshold is built from quantities the sweep *does* estimate well:

1. The per-variant null Sharpe distribution, from all 1512 runs. It comes out
   close to normal -- skew -0.26, excess kurtosis +0.06 -- which is what licenses
   the rest.
2. The maximum of n draws from it is Gumbel, with a location and scale that
   follow from the normal's tail.
3. The observed mean best-of-252 calibrates *n*: 252 correlated variants behave
   like about 81 independent ones. Variants that share a feature or a horizon are
   not separate tries, and assuming they were would put the bar too high.
4. The threshold is the 95th percentile of that calibrated distribution.

The resampled table is still reported, as a floor and as a check on step 3.
"""

from __future__ import annotations

import argparse
import itertools
import json
import time
from pathlib import Path

import numpy as np
import pandas as pd
from scipy import stats

from research import metrics
from research.features import Feature
from research.panel import PanelSpec, generate_panel
from research.pipeline import PipelineConfig, run_pipeline
from research.portfolio import PortfolioConstraints

REPO = Path(__file__).resolve().parent.parent
RESULTS = REPO / "docs" / "figures" / "false_discovery.json"
FIGURE = REPO / "docs" / "figures" / "false_discovery.png"

NULL_SPEC = PanelSpec(
    n_securities=100,
    n_periods=750,
    lam=0.0,
    seed=0,  # replaced per panel
    delist_hazard=0.0005,
    delist_exposure_tilt=0.0,
)
"""Three years of a hundred names, with churn, and **no signal at all**.

Churn is on because a null panel with no departures is a friendlier null than the
real problem: forced exits give the portfolio layer something to do, and a book
that trades for reasons unrelated to its score is part of what generates spurious
Sharpes.

**The hazard tilt is off, and that is load-bearing.** The first version of this
spec carried a tilt of 0.3, and it made the panel not null. A departure hazard
that depends on the exposure means low-exposure names die more often, a departing
name's final period is -30%, and the exposure therefore predicts forward returns
through the departure channel -- measurably: across three seeds the exposure's
t-statistic against forward returns came out +1.16, +2.04 and +0.80, all
positive, where an untilted panel gave +0.61, +1.22 and -0.00.

A real signal of that size in the panel would have inflated the false-discovery
rate this milestone reports, and the inflation would have been indistinguishable
from the thing being measured. `test_sweep.py::test_the_null_panel_is_actually_
null` pins it, because the check is not one anybody would think to repeat.
"""

DEFAULT_PANELS = 6
CONVENTIONAL_T = 1.96
STRICT_T = 2.576

_SURFACE = "#fcfcfb"
_INK = "#0b0b0b"
_MUTED = "#52514e"
_SERIES = "#2a78d6"
_WARN = "#eb6834"
_GRID = "#d9d8d4"

EULER_MASCHERONI = 0.5772156649015329


# ---------------------------------------------------------------------------
# The variant family
# ---------------------------------------------------------------------------

_FEATURE_SETS: tuple[tuple[str, tuple[Feature, ...]], ...] = (
    ("exposure", (Feature("exposure", "level", 1),)),
    ("exposure.rank", (Feature("exposure", "level", 1, cross_section="rank"),)),
    ("exposure.mean5", (Feature("exposure", "mean", 5),)),
    ("exposure.mean20", (Feature("exposure", "mean", 20),)),
    ("exposure.mean60", (Feature("exposure", "mean", 60),)),
    ("exposure.ewm10", (Feature("exposure", "ewm", 10),)),
    ("exposure.change20", (Feature("exposure", "change", 20),)),
    ("ret.mean5", (Feature("ret", "mean", 5),)),
    ("ret.mean20", (Feature("ret", "mean", 20),)),
    ("ret.mean60", (Feature("ret", "mean", 60),)),
    ("ret.compound20", (Feature("ret", "compound", 20),)),
    ("ret.sd20", (Feature("ret", "sd", 20),)),
    ("ret.zscore60", (Feature("ret", "zscore", 60),)),
    ("ret.rank60", (Feature("ret", "rank", 60),)),
    ("volume.mean20", (Feature("volume", "mean", 20),)),
    ("volume.zscore60", (Feature("volume", "zscore", 60),)),
    ("price.rank60", (Feature("price", "rank", 60),)),
    ("exposure+ret", (Feature("exposure", "level", 1), Feature("ret", "mean", 20))),
    ("exposure+vol", (Feature("exposure", "level", 1), Feature("ret", "sd", 20))),
    ("ret+volume", (Feature("ret", "mean", 20), Feature("volume", "mean", 20))),
    ("three", (Feature("exposure", "mean", 5), Feature("ret", "sd", 20),
               Feature("volume", "zscore", 60))),
)
"""Twenty-one feature sets, all generic transforms of panel columns.

Several of these are recognisable financial factors if you squint -- an average of
past returns is a well-known one -- and that is deliberate and harmless: on a null
panel none of them can be anything but a transform of noise. What they provide is
*plausibility*, because a sweep over obviously silly variants would understate how
many reasonable-looking things an analyst tries.
"""

_HORIZONS = (1, 5, 20)
_MODELS: tuple[tuple[str, dict[str, float | int]], ...] = (
    ("ridge", {"alpha": 1.0}),
    ("ridge", {"alpha": 100.0}),
    ("gbm", {"max_depth": 3}),
    ("gbm", {"max_depth": 6, "learning_rate": 0.1}),
)


def variants() -> list[PipelineConfig]:
    """Every combination. Order is fixed, so a sweep is reproducible."""
    out: list[PipelineConfig] = []
    for (name, features), horizon, (model, kwargs) in itertools.product(
            _FEATURE_SETS, _HORIZONS, _MODELS):
        suffix = "".join(f",{key}={value:g}" for key, value in sorted(kwargs.items()))
        out.append(PipelineConfig(
            features=features,
            horizon=horizon,
            model=model,
            model_kwargs=dict(kwargs),
            embargo=max(horizon, 5),
            constraints=PortfolioConstraints(max_weight=0.04, turnover_budget=0.25),
            rebalance_every=21,
            label=f"{name}|h{horizon}|{model}{suffix}",
        ))
    return out


# ---------------------------------------------------------------------------
# Running it
# ---------------------------------------------------------------------------


def run_sweep(panels: int = DEFAULT_PANELS, spec: PanelSpec = NULL_SPEC,
              configs: list[PipelineConfig] | None = None,
              verbose: bool = True) -> pd.DataFrame:
    """One row per (panel, variant). Slow by design: this is the measurement."""
    configs = configs or variants()
    if spec.lam != 0.0:
        raise ValueError(f"the false-discovery sweep requires lam = 0; got {spec.lam}")

    rows: list[dict[str, object]] = []
    started = time.time()
    for panel_index in range(panels):
        seed = 20260918 + panel_index
        panel = generate_panel(spec.replace(seed=seed))
        for variant_index, config in enumerate(configs):
            result = run_pipeline(panel, config)
            years = len(result.ic) / metrics.ANNUAL_PERIODS if len(result.ic) else np.nan
            sharpe = result.sharpe
            rows.append({
                "panel": panel_index,
                "seed": seed,
                "variant": config.label,
                "variant_index": variant_index,
                "model": config.model,
                "horizon": config.horizon,
                "features": "+".join(f.label for f in config.features),
                "ic_mean": result.ic_mean,
                "ic_t": result.ic_t,
                "lambda_t": result.lambda_hat.t_stat,
                "oos_r2": result.oos_r2,
                "sharpe": sharpe,
                # A Sharpe's own t-statistic: annualized Sharpe times the square
                # root of the sample in years. The quantity anybody comparing a
                # Sharpe to 1.96 is implicitly forming.
                "sharpe_t": sharpe * np.sqrt(years) if np.isfinite(sharpe) else np.nan,
                "turnover": (float(result.portfolio.turnover.mean())
                             if result.portfolio is not None else np.nan),
                "n_rows": result.n_rows,
            })
            if verbose and (len(rows) % 25 == 0):
                done = len(rows)
                total = panels * len(configs)
                rate = (time.time() - started) / done
                print(f"  {done}/{total} runs, {rate * (total - done) / 60:.1f} min left",
                      flush=True)
    return pd.DataFrame(rows)


# ---------------------------------------------------------------------------
# What came out
# ---------------------------------------------------------------------------


def false_discovery_rate(frame: pd.DataFrame, threshold: float = CONVENTIONAL_T
                         ) -> dict[str, float]:
    """The fraction of variants that claimed a finding, per statistic.

    **The lambda estimate is deduplicated first, and that matters.** `lambda_hat`
    is measured against the panel's raw exposure, so it does not depend on a
    variant's features or model at all -- only on the panel, the label horizon,
    and how many rows the feature windows warmed up over. In the committed sweep
    that leaves **108 distinct estimates** behind 1512 runs, and counting the
    copies would report a rate on a denominator that is mostly itself.

    Deduplication is by exact value rather than by (panel, horizon), because the
    warm-up length genuinely does move the estimate a little and asserting
    otherwise would be asserting something false. The 108 are still far from
    independent -- six warm-ups per (panel, horizon) differing only in where the
    sample starts -- so read the denominator as an upper bound on the trials.
    """
    out: dict[str, float] = {}
    for column in ("ic_t", "sharpe_t"):
        values = frame[column].dropna()
        out[column] = float((values.abs() > threshold).mean()) if len(values) else float("nan")
        out[f"{column}_positive"] = (float((values > threshold).mean()) if len(values)
                                     else float("nan"))
        out[f"{column}_n"] = float(len(values))

    distinct = frame.drop_duplicates(subset=["panel", "horizon", "lambda_t"])["lambda_t"].dropna()
    out["lambda_t"] = float((distinct.abs() > threshold).mean()) if len(distinct) else float("nan")
    out["lambda_t_positive"] = (float((distinct > threshold).mean()) if len(distinct)
                                else float("nan"))
    out["lambda_t_n"] = float(len(distinct))
    return out


def best_of_n(frame: pd.DataFrame, sizes: tuple[int, ...] = (1, 10, 50, 100, 200),
              draws: int = 4000, seed: int = 20260918) -> pd.DataFrame:
    """The distribution of the best Sharpe among N variants tried.

    Resampled within panel, so that variants sharing a feature or a horizon stay
    as correlated in the estimate as they are in practice. Drawing across panels
    instead would treat them as independent tries and overstate the threshold.
    """
    rng = np.random.default_rng(seed)
    by_panel = [group["sharpe"].dropna().to_numpy()
                for _, group in frame.groupby("panel", sort=True)]
    by_panel = [values for values in by_panel if values.size]
    if not by_panel:
        raise ValueError("no finite Sharpes in the sweep")

    rows = []
    for size in sizes:
        maxima = np.empty(draws)
        for draw in range(draws):
            values = by_panel[rng.integers(len(by_panel))]
            picked = rng.choice(values, size=min(size, values.size),
                                replace=size > values.size)
            maxima[draw] = picked.max()
        rows.append({
            "n": size,
            "mean": float(maxima.mean()),
            "median": float(np.median(maxima)),
            "p90": float(np.percentile(maxima, 90)),
            "p95": float(np.percentile(maxima, 95)),
            "p99": float(np.percentile(maxima, 99)),
            "max": float(maxima.max()),
        })
    return pd.DataFrame(rows)


def _gumbel_parameters(sigma: float, n: float) -> tuple[float, float]:
    """Location and scale of the limiting distribution of the maximum.

    For n draws from N(0, sigma), the maximum is asymptotically Gumbel with
    location `sigma * z` and scale `sigma / (n * phi(z))`, where `z` is the
    1 - 1/n quantile of the standard normal. Textbook, and the basis of
    everything below.
    """
    z = float(stats.norm.ppf(1.0 - 1.0 / n))
    return sigma * z, sigma / (n * float(stats.norm.pdf(z)))


def expected_max_sharpe(sigma: float, n: int) -> float:
    """Expected maximum of n draws from N(0, sigma).

    Assumes the n tries are independent and normally distributed. The tries in a
    real sweep are neither, which is what `effective_tries` is for.
    """
    if n < 2 or not np.isfinite(sigma) or sigma <= 0.0:
        return metrics.UNDEFINED
    location, scale = _gumbel_parameters(sigma, n)
    return float(location + EULER_MASCHERONI * scale)


def max_sharpe_quantile(sigma: float, n: float, percentile: float = 0.95) -> float:
    """A quantile of the best-of-n distribution, not just its mean.

    A threshold set at the *expected* maximum is the median-ish of what a search
    on nothing produces, which would call half of all pure-noise searches a
    discovery. The quantile is the number a threshold should be.
    """
    if n < 2 or not np.isfinite(sigma) or sigma <= 0.0:
        return metrics.UNDEFINED
    location, scale = _gumbel_parameters(sigma, n)
    return float(location + scale * -np.log(-np.log(percentile)))


def effective_tries(sigma: float, mean_best: float, n: int) -> float:
    """How many *independent* tries the observed best-of-n behaves like.

    The variants in this sweep are not independent -- they share features,
    horizons and models -- so `n` of them explore less of the space than `n`
    independent draws would. Solving the expected-maximum relation backwards for
    the number of draws that would produce the observed mean best puts a figure
    on that, and it is the figure the threshold should use: assuming 252
    independent tries when they behave like 85 sets the bar too high, which is a
    different way of being wrong from setting it too low, and not a better one.

    Returns `n` unchanged if the observed mean best already exceeds what `n`
    independent draws would give -- a signal that the null Sharpes have a fatter
    tail than the normal this is built on, which the caller should report rather
    than absorb.
    """
    if not np.isfinite(sigma) or sigma <= 0.0 or not np.isfinite(mean_best):
        return float("nan")
    if mean_best >= expected_max_sharpe(sigma, n):
        return float(n)

    from scipy import optimize

    def gap(log_n: float) -> float:
        return expected_max_sharpe(sigma, float(np.exp(log_n))) - mean_best

    low, high = np.log(2.0001), np.log(float(n))
    if gap(low) > 0.0:
        return 2.0  # even two tries overshoot: nothing to calibrate
    return float(np.exp(optimize.brentq(gap, low, high, xtol=1e-6)))


def summarize(frame: pd.DataFrame) -> dict[str, object]:
    """Everything the report and the threshold constant are derived from.

    WHY THE THRESHOLD IS NOT THE RESAMPLED 95TH PERCENTILE

    That was the first design and it does not work, for a reason visible in the
    numbers rather than in theory. Drawing 252 variants from a panel that has
    exactly 252 of them returns the panel's maximum every time, so the best-of-252
    "distribution" has one realization per panel -- six of them -- and its 90th,
    95th and 99th percentiles are all the largest Sharpe observed anywhere in the
    sweep. That is a floor, not a percentile, and quoting it as one would state a
    number with far more confidence than six observations support.

    So the threshold is built the other way round. The *per-variant* null Sharpe
    distribution is estimated from all 1512 runs and is close to normal; the
    maximum of n draws from it is Gumbel; the observed mean best-of-252 calibrates
    how many independent tries 252 correlated ones are worth; and the threshold is
    the 95th percentile of that calibrated distribution. Every input is a
    quantity the sweep estimates well.
    """
    sharpes = frame["sharpe"].dropna()
    sigma = float(sharpes.std(ddof=1))
    n_variants = int(frame["variant"].nunique())
    best = best_of_n(frame, sizes=(1, 10, 50, 100, n_variants))
    panel_maxima = frame.groupby("panel")["sharpe"].max()

    at_n = best.loc[best["n"] == n_variants].iloc[0]
    mean_best = float(panel_maxima.mean())
    n_effective = effective_tries(sigma, mean_best, n_variants)

    return {
        "runs": int(len(frame)),
        "panels": int(frame["panel"].nunique()),
        "variants": n_variants,
        "sharpe_mean": float(sharpes.mean()),
        "sharpe_sd": sigma,
        "sharpe_max": float(sharpes.max()),
        "sharpe_min": float(sharpes.min()),
        "sharpe_skew": float(sharpes.skew()),
        "sharpe_kurtosis": float(sharpes.kurtosis()),
        "fdr_conventional": false_discovery_rate(frame, CONVENTIONAL_T),
        "fdr_strict": false_discovery_rate(frame, STRICT_T),
        "fdr_by_horizon": {
            int(horizon): false_discovery_rate(group, CONVENTIONAL_T)
            for horizon, group in frame.groupby("horizon")
        },
        "best_of_n": best.to_dict(orient="records"),
        "panel_maxima": [float(value) for value in panel_maxima],
        "mean_best": mean_best,
        "sd_best": float(panel_maxima.std(ddof=1)),
        "resampled_p95_floor": float(at_n["p95"]),
        "n_effective": n_effective,
        "threshold": max_sharpe_quantile(sigma, n_effective, 0.95),
        "threshold_independent": max_sharpe_quantile(sigma, n_variants, 0.95),
        "expected_max_analytic": expected_max_sharpe(sigma, n_variants),
    }


def plot_sweep(frame: pd.DataFrame, summary: dict[str, object], path: Path = FIGURE) -> Path:
    """The null Sharpe distribution, with the best-of-N threshold marked."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    sharpes = frame["sharpe"].dropna()
    threshold = float(summary["threshold"])

    figure, axes = plt.subplots(figsize=(7.2, 4.6), dpi=200)
    figure.patch.set_facecolor(_SURFACE)
    axes.set_facecolor(_SURFACE)

    axes.hist(sharpes, bins=48, color=_SERIES, alpha=0.85, edgecolor=_SURFACE, linewidth=0.6,
              label="one pipeline variant on a null panel")
    axes.axvline(threshold, color=_WARN, linewidth=2.0,
                 label=f"best-of-{summary['variants']} threshold ({threshold:.2f})")
    axes.axvline(0.0, color=_MUTED, linewidth=1.0, linestyle=(0, (4, 3)))

    axes.set_title("Sharpe ratios found in data containing no signal",
                   color=_INK, fontsize=13, pad=26, loc="left")
    axes.text(0.0, 1.022,
              f"{summary['runs']} runs: {summary['variants']} variants over "
              f"{summary['panels']} panels, lambda = 0",
              transform=axes.transAxes, color=_MUTED, fontsize=9.5)
    axes.set_xlabel("annualized Sharpe ratio (frictionless)", color=_MUTED, fontsize=10)
    axes.set_ylabel("variants", color=_MUTED, fontsize=10)

    axes.grid(True, axis="y", color=_GRID, linewidth=0.8, alpha=0.9)
    axes.set_axisbelow(True)
    for side in ("top", "right"):
        axes.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        axes.spines[side].set_color(_GRID)
    axes.tick_params(colors=_MUTED, labelsize=9)

    legend = axes.legend(loc="upper left", frameon=False, fontsize=9)
    for text in legend.get_texts():
        text.set_color(_MUTED)

    figure.tight_layout()
    figure.savefig(path, facecolor=_SURFACE)
    plt.close(figure)
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="M8: false discovery rate")
    parser.add_argument("--panels", type=int, default=DEFAULT_PANELS)
    parser.add_argument("--out", type=Path, default=RESULTS)
    parser.add_argument("--quick", action="store_true",
                        help="eight variants on two small panels; checks the machinery, "
                             "measures nothing")
    args = parser.parse_args(argv)

    configs = variants()
    spec = NULL_SPEC
    panels = args.panels
    if args.quick:
        configs = configs[::len(configs) // 8][:8]
        spec = spec.replace(n_securities=60, n_periods=400)
        panels = 2

    print(f"sweep: {len(configs)} variants x {panels} panels = {len(configs) * panels} runs")
    frame = run_sweep(panels=panels, spec=spec, configs=configs)
    summary = summarize(frame)

    print()
    print(f"  null Sharpe:      mean {summary['sharpe_mean']:+.3f}, "
          f"sd {summary['sharpe_sd']:.3f}, "
          f"range {summary['sharpe_min']:+.2f} to {summary['sharpe_max']:+.2f}")
    fdr = summary["fdr_conventional"]
    print(f"  false discovery:  IC t {fdr['ic_t']:.1%}, "
          f"lambda t {fdr['lambda_t']:.1%}, Sharpe t {fdr['sharpe_t']:.1%}  (|t| > 1.96)")
    print(f"  best-of-{summary['variants']}:      observed mean {summary['mean_best']:+.2f} "
          f"over {summary['panels']} panels, behaving like "
          f"{summary['n_effective']:.0f} independent tries")
    print(f"  THRESHOLD:        {summary['threshold']:+.2f}   "
          f"(assuming independence: {summary['threshold_independent']:+.2f}; "
          f"resampled floor: {summary['resampled_p95_floor']:+.2f})")
    print()
    print(pd.DataFrame(summary["best_of_n"]).to_string(index=False, float_format="%+.3f"))

    if not args.quick:
        path = Path(args.out)
        path.parent.mkdir(parents=True, exist_ok=True)
        # Every run, but only the columns a re-analysis would need, rounded.
        # `features` duplicates `variant`, and six decimal places is more
        # precision than any of these statistics has. A committed artifact should
        # be the evidence for the number above it, not a database.
        kept = ["panel", "seed", "variant", "model", "horizon",
                "ic_mean", "ic_t", "lambda_t", "oos_r2", "sharpe", "sharpe_t",
                "turnover", "n_rows"]
        runs = frame[kept].round(6)
        path.write_text(json.dumps({
            "spec": spec.to_dict(),
            "summary": summary,
            "runs": runs.to_dict(orient="records"),
        }, indent=2, default=str), encoding="utf-8")
        print(f"\nwrote {path}")
        print(f"wrote {plot_sweep(frame, summary)}")

        from research.sweep_report import write_report
        print(f"wrote {write_report(frame, summary)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
