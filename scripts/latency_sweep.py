#!/usr/bin/env python3
"""Measures what latency does to slippage, across many synthetic price paths.

The one-path observation this replaces was that `sma_crossover` reported a
smaller loss with 250ms of latency than without it. That is not a finding, and
the reasoning for why it is not -- synthetic GBM has no short-horizon structure,
so drift over a flight window is zero-mean and latency makes slippage two-sided
with unchanged expectation -- is an argument, not a measurement.

So this measures it. The same comparison is run across N seeds and the
distribution of the difference is reported. The expectation is that it
straddles zero. A distribution centred away from zero would mean the simulator
is doing something systematic, which would be a bug worth finding rather than a
note worth writing.

Runs the headless binary rather than importing the Python module: the binary is
what the numbers in the documentation come from, and this should measure that
rather than something adjacent to it.

    python scripts/latency_sweep.py                    # 200 seeds, default preset
    python scripts/latency_sweep.py --seeds 500 --latency 250
"""

from __future__ import annotations

import argparse
import concurrent.futures
import math
import re
import statistics
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# "  SMA Crossover            98647.11   -1.35%   -17.13 ..." -- the theory name
# runs to a column boundary, so the split is on two-or-more spaces rather than
# on whitespace, or "SMA Crossover" becomes two fields.
ROW = re.compile(r"^\s{2}(\S.*?)\s{2,}(-?[\d.]+)\s")


@dataclass(frozen=True)
class Row:
    equity: float
    slippage: float
    trades: int


def find_binary(preset: str) -> Path:
    exe = REPO / "out" / "build" / preset / "bin" / "marketscan.exe"
    if not exe.exists():
        raise SystemExit(
            f"No binary at {exe}.\n"
            f"  ./scripts/dev-env.ps1 cmake --build --preset {preset}"
        )
    return exe


def parse(output: str) -> dict[str, Row]:
    """Pulls the results table out of the headless run."""
    rows: dict[str, Row] = {}
    in_table = False
    for line in output.splitlines():
        if line.startswith("Results (spread-crossing fills)"):
            in_table = True
            continue
        if in_table and line.startswith("  theory"):
            continue
        if in_table and not line.strip():
            if rows:
                break
            continue
        if not in_table:
            continue
        # theory | equity | return | sharpe | sortino | max dd | IR | trades | slippage
        fields = re.split(r"\s{2,}", line.strip())
        if len(fields) < 9:
            continue
        try:
            rows[fields[0]] = Row(
                equity=float(fields[1].replace(",", "")),
                slippage=float(fields[8].replace(",", "")),
                trades=int(fields[7]),
            )
        except ValueError:
            continue
    return rows


def run_once(exe: Path, seed: int, latency_ms: float, jitter_ms: float) -> tuple[dict, dict]:
    """One seed, with and without latency, over freshly generated data."""

    def invoke(latency: float, jitter: float) -> dict[str, Row]:
        command = [
            str(exe),
            "--headless",
            "--seed", str(seed),
            "--sessions", "4",
            "--bars", "390",
        ]
        if latency > 0:
            command += ["--latency", str(latency), "--latency-jitter", str(jitter)]
        result = subprocess.run(
            command, capture_output=True, text=True, cwd=REPO, check=False
        )
        if result.returncode != 0:
            raise RuntimeError(f"seed {seed} exited {result.returncode}: {result.stderr[:200]}")
        return parse(result.stdout)

    return invoke(0.0, 0.0), invoke(latency_ms, jitter_ms)


def describe(name: str, values: list[float], helps_when_negative: bool) -> list[str]:
    """Summary statistics, with the sign convention stated rather than assumed.

    Less slippage is good and less equity is not, so which direction counts as
    favourable is the caller's business. Getting that backwards would put a
    number in a table that reads as its own opposite.
    """
    if not values:
        return [f"{name}: no observations"]
    unit = "$"
    mean = statistics.fmean(values)
    sd = statistics.stdev(values) if len(values) > 1 else 0.0
    stderr = sd / math.sqrt(len(values)) if values else 0.0
    favourable = sum(1 for v in values if (v < 0.0) == helps_when_negative) / len(values)
    return [
        f"| observations | {len(values)} |",
        f"| mean | {unit}{mean:,.2f} |",
        f"| standard deviation | {unit}{sd:,.2f} |",
        f"| standard error of the mean | {unit}{stderr:,.2f} |",
        f"| mean / standard error | {mean / stderr if stderr else float('nan'):.2f} |",
        f"| median | {unit}{statistics.median(values):,.2f} |",
        f"| minimum | {unit}{min(values):,.2f} |",
        f"| maximum | {unit}{max(values):,.2f} |",
        f"| fraction of seeds where latency helped | {favourable:.1%} |",
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seeds", type=int, default=200)
    parser.add_argument("--latency", type=float, default=250.0, help="milliseconds")
    parser.add_argument("--jitter", type=float, default=100.0, help="milliseconds")
    parser.add_argument("--preset", default="ci")
    parser.add_argument("--theory", default="SMA Crossover")
    parser.add_argument("--out", type=Path, default=REPO / "docs" / "LATENCY_DISTRIBUTION.md")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()

    exe = find_binary(args.preset)
    print(f"{args.seeds} seeds through {exe.name}, {args.latency}ms +/- {args.jitter}ms")

    slippage_delta: list[float] = []
    equity_delta: list[float] = []
    trade_counts: list[int] = []
    failures = 0

    # Each run is an independent process, so this is embarrassingly parallel and
    # the whole sweep is a couple of minutes rather than a quarter of an hour.
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(run_once, exe, seed, args.latency, args.jitter): seed
            for seed in range(1, args.seeds + 1)
        }
        for done, future in enumerate(concurrent.futures.as_completed(futures), 1):
            try:
                instant, delayed = future.result()
            except RuntimeError as error:
                failures += 1
                print(f"  {error}", file=sys.stderr)
                continue
            if args.theory not in instant or args.theory not in delayed:
                failures += 1
                continue
            a, b = instant[args.theory], delayed[args.theory]
            # Negative means latency cost less slippage than no latency, which
            # is the direction the one-path observation went.
            slippage_delta.append(b.slippage - a.slippage)
            equity_delta.append(b.equity - a.equity)
            trade_counts.append(a.trades)
            if done % 25 == 0:
                print(f"  {done}/{args.seeds}")

    if not slippage_delta:
        raise SystemExit("every run failed; nothing to report")

    lines = [
        f"# What latency does to slippage, across {args.seeds} price paths",
        "",
        "Generated by `scripts/latency_sweep.py`. Regenerate with:",
        "",
        "```powershell",
        f"python scripts/latency_sweep.py --seeds {args.seeds} "
        f"--latency {args.latency} --jitter {args.jitter}",
        "```",
        "",
        "## What is being measured",
        "",
        f"For each of {args.seeds} seeds, the synthetic generator produces a different price",
        f"path (4 sessions of 390 one-minute bars). "
        f"`{args.theory}` is run over it twice:",
        "once with an instant fill simulator and once with "
        f"{args.latency:g}ms of latency plus up to {args.jitter:g}ms of jitter. The difference in",
        "the slippage column is recorded.",
        "",
        "**Negative means latency was favourable** -- less slippage paid than without it, which",
        "is the direction the single-path observation in `EXECUTION.md` went.",
        "",
        "## Slippage difference",
        "",
        "| | |",
        "|---|---|",
        *describe("slippage", slippage_delta, helps_when_negative=True),
        "",
        "## Equity difference",
        "",
        "| | |",
        "|---|---|",
        *describe("equity", equity_delta, helps_when_negative=False),
        "",
    ]

    mean = statistics.fmean(slippage_delta)
    sd = statistics.stdev(slippage_delta) if len(slippage_delta) > 1 else 0.0
    stderr = sd / math.sqrt(len(slippage_delta))
    t_stat = mean / stderr if stderr else float("nan")
    straddles = min(slippage_delta) < 0.0 < max(slippage_delta)
    favourable = sum(1 for v in slippage_delta if v < 0.0) / len(slippage_delta)

    lines += [
        "## Reading it",
        "",
    ]
    if straddles and abs(t_stat) < 3.0:
        lines += [
            f"The distribution straddles zero, and the mean is {abs(t_stat):.1f} standard errors",
            "from it. That is what synthetic geometric Brownian motion should produce: price",
            "drift over a flight window of a few hundred milliseconds is zero-mean by",
            "construction, so latency makes slippage two-sided -- unchanged expectation, added",
            "variance -- rather than one-sided.",
            "",
            f"Latency looked favourable on {favourable:.0%} of paths. The single-path result",
            "quoted in `EXECUTION.md` was one of those, and reading it as a property of latency",
            "would have been reading one draw from this distribution as its mean.",
        ]
    else:
        lines += [
            f"**The distribution does not straddle zero as expected.** Mean {mean:,.2f}, "
            f"{abs(t_stat):.1f} standard errors from zero.",
            "",
            "That is not what synthetic GBM should produce, and it means the simulator is doing",
            "something systematic with latency rather than merely adding variance. This is a",
            "finding about the engine, not about markets, and it should be investigated before",
            "any result produced with latency enabled is trusted.",
        ]

    lines += [
        "",
        "**None of this generalises to real data.** Latency acquires a sign when returns have",
        "short-horizon structure: a signal that fires on a move gets systematically worse fills",
        "during flight in a market with continuation, and better ones in a mean-reverting",
        "market. Synthetic GBM has neither, which is exactly why it shows neither. The sign and",
        "magnitude on real data is an empirical property of the strategy and its horizon, and",
        "nothing here is entitled to an opinion about it.",
        "",
    ]

    if failures:
        lines += [f"> {failures} of {args.seeds} runs produced no usable row and were skipped.", ""]

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines), encoding="utf-8")

    print()
    print(f"slippage delta: mean {mean:,.2f}, sd {sd:,.2f}, t {t_stat:.2f}")
    print(f"favourable on {favourable:.1%} of {len(slippage_delta)} paths")
    try:
        print(f"wrote {args.out.relative_to(REPO)}")
    except ValueError:
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
