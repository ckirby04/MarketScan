"""What moves the bar: sample length against variant discipline.

    python -m research.sample_length            # the committed run
    python -m research.sample_length --quick

`docs/FALSE_DISCOVERY.md` reports a Sharpe bar of 1.96 for a 252-variant search
over three years, and says honestly that a bar that high cannot separate a real
signal from the best of its own noise. A reader's next question is what to do
about it, and there are only two levers: test fewer things, or have more data.

This measures both, so the answer is not an argument.

THE ARITHMETIC IT IS CHECKING

The standard error of an annualized Sharpe over T years is roughly 1/sqrt(T), so
the spread of null Sharpes should shrink as the sample grows -- and the M8 sweep
already agrees with that to three digits at three years: 0.5785 measured against
0.5774 predicted. The bar is that spread multiplied by an extreme-value factor
that grows only *logarithmically* with the number of tries.

Logarithmic against square-root is the whole point. Cutting a search from 252
variants to 25 divides the multiplier by a little; lengthening the sample from
three years to twenty divides the spread by more than two. If that holds, sample
length dominates variant discipline and the project's binding constraint is a
data subscription rather than a modelling idea.

WHAT IS MEASURED AND WHAT IS PROJECTED

Measured, at each sample length: the null Sharpe spread, the observed mean
best-of-25, the effective independent tries that implies, and the resulting bar.

Projected, for the 252-variant column: the same arithmetic using the effective
try count measured for 252 variants in M8. That projection rests on effective
tries being a property of the *family* rather than of the sample length, which is
an assumption -- so the effective count for 25 variants is reported at every
length, and if it drifts, the projection is worth less than the measurement
beside it.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import pandas as pd

from research import metrics
from research.panel import PanelSpec
from research.sweep import (
    NULL_SPEC,
    effective_tries,
    max_quantile,
    run_sweep,
    variants,
)
from research.thresholds import NULL_EFFECTIVE_TRIES, NULL_SWEEP_VARIANTS

REPO = Path(__file__).resolve().parent.parent
RESULTS = REPO / "docs" / "figures" / "sample_length.json"

SAMPLE_YEARS = (3, 8, 20)
"""Three is what the project has. Twenty is what CRSP would give. Eight is in
between, so that the scaling is measured on three points rather than asserted
from two."""

VARIANT_COUNT = 25
"""A disciplined search, for the column that answers "what if I tried fewer
things". Spread across the family rather than taken from the front of it, so it
spans feature sets, horizons and both model types."""

PANELS = 4
"""Fewer than M8's six. A twenty-year panel costs about ten times a three-year
one to run, and the quantity these panels are estimating -- a standard deviation
across 25 variants -- is far better determined than the maximum M8 needed six
panels for."""


def scaling_variants(count: int = VARIANT_COUNT) -> list:
    configs = variants()
    step = max(len(configs) // count, 1)
    return configs[::step][:count]


def run_scaling(years: tuple[int, ...] = SAMPLE_YEARS, panels: int = PANELS,
                count: int = VARIANT_COUNT, spec: PanelSpec = NULL_SPEC,
                verbose: bool = True) -> pd.DataFrame:
    """The null sweep at several sample lengths. One frame, with a `years` column."""
    configs = scaling_variants(count)
    frames = []
    for span in years:
        periods = int(round(span * metrics.ANNUAL_PERIODS))
        if verbose:
            print(f"--- {span} years ({periods} periods) x {len(configs)} variants "
                  f"x {panels} panels", flush=True)
        frame = run_sweep(panels=panels, spec=spec.replace(n_periods=periods),
                          configs=configs, verbose=verbose)
        frames.append(frame.assign(years=span, periods=periods))
    return pd.concat(frames, ignore_index=True)


def scaling_table(frame: pd.DataFrame, percentile: float = 0.95) -> pd.DataFrame:
    """One row per sample length: what the bar is, measured and projected."""
    rows = []
    for span, group in frame.groupby("years", sort=True):
        sharpes = group["sharpe"].dropna()
        if sharpes.empty:
            continue
        sigma = float(sharpes.std(ddof=1))
        count = int(group["variant"].nunique())

        per_panel_best = group.groupby("panel")["sharpe"].max()
        mean_best = float(per_panel_best.mean())
        n_effective = effective_tries(sigma, mean_best, count)

        rows.append({
            "years": int(span),
            "periods": int(group["periods"].iloc[0]),
            "runs": int(len(sharpes)),
            "variants": count,
            "sharpe_sd": sigma,
            "sharpe_sd_predicted": float(1.0 / np.sqrt(span)),
            "mean_best": mean_best,
            "n_effective": float(n_effective),
            "bar_small_search": max_quantile(sigma, n_effective, percentile),
            # The wide-search column, using the effective try count M8 measured
            # for the full family. See the module docstring on what this assumes.
            "bar_full_search": max_quantile(sigma, NULL_EFFECTIVE_TRIES, percentile),
        })
    return pd.DataFrame(rows)


def summarize(frame: pd.DataFrame) -> dict[str, object]:
    table = scaling_table(frame)
    drift = (table["n_effective"].max() - table["n_effective"].min()) / max(
        table["n_effective"].mean(), 1e-9)
    return {
        "runs": int(len(frame)),
        "panels": int(frame["panel"].nunique()),
        "variants": int(frame["variant"].nunique()),
        "full_search_variants": NULL_SWEEP_VARIANTS,
        "full_search_effective": NULL_EFFECTIVE_TRIES,
        "years": [int(y) for y in table["years"]],
        "effective_tries_drift": float(drift),
        "by_years": table.to_dict(orient="records"),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Part 2: what moves the bar")
    parser.add_argument("--out", type=Path, default=RESULTS)
    parser.add_argument("--panels", type=int, default=PANELS)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args(argv)

    years = SAMPLE_YEARS
    count = VARIANT_COUNT
    panels = args.panels
    if args.quick:
        years = (1, 3)
        count = 6
        panels = 2

    frame = run_scaling(years=years, panels=panels, count=count)
    summary = summarize(frame)
    table = pd.DataFrame(summary["by_years"])

    print()
    print(table.to_string(index=False, formatters={
        "sharpe_sd": "{:.4f}".format,
        "sharpe_sd_predicted": "{:.4f}".format,
        "mean_best": "{:+.3f}".format,
        "n_effective": "{:.0f}".format,
        "bar_small_search": "{:+.3f}".format,
        "bar_full_search": "{:+.3f}".format,
    }))
    print(f"\neffective-tries drift across sample lengths: "
          f"{summary['effective_tries_drift']:.1%}")

    if not args.quick:
        path = Path(args.out)
        path.parent.mkdir(parents=True, exist_ok=True)
        kept = ["panel", "years", "periods", "variant", "model", "horizon", "sharpe",
                "sharpe_t", "ic_mean", "n_rows"]
        path.write_text(json.dumps({
            "spec": NULL_SPEC.to_dict(),
            "summary": summary,
            "runs": frame[kept].round(6).to_dict(orient="records"),
        }, indent=2, default=str), encoding="utf-8")
        print(f"wrote {path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
