#!/usr/bin/env python3
"""Generates bindings/python/examples/quickstart.ipynb.

The notebook is the committed artifact; this script exists so its source is
diffable and so the cells can be edited as code rather than as escaped JSON.
Re-run it, then execute the notebook, whenever the bindings change:

    python scripts/make_quickstart_notebook.py
    python scripts/run_notebook.py
"""

from __future__ import annotations

import json
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TARGET = REPO / "bindings" / "python" / "examples" / "quickstart.ipynb"


# Stable, deterministic cell ids. nbformat 4.5 requires them, and random ones
# would make every regeneration a diff against the committed copy.
_next_id = 0


def _cell_id() -> str:
    global _next_id
    _next_id += 1
    return f"marketscan-{_next_id:02d}"


def md(text: str) -> dict:
    return {
        "cell_type": "markdown",
        "id": _cell_id(),
        "metadata": {},
        "source": text.splitlines(keepends=True),
    }


def code(text: str) -> dict:
    return {
        "cell_type": "code",
        "id": _cell_id(),
        "execution_count": None,
        "metadata": {},
        "outputs": [],
        "source": text.rstrip("\n").splitlines(keepends=True),
    }


CELLS = [
    md(
        """# MarketScan quickstart

MarketScan is a C++ engine for running trading strategies against market data.
This notebook runs one, gets the numbers out, and shows the single result the
whole design exists to make visible.

**Strategies are not written here.** A strategy is a compiled plugin dropped
into `theories/`, because the engine's contract is that a strategy is a pure
function of the events it was shown -- and a Python callback on the per-event
path would make replay speed depend on the interpreter and let a strategy close
over state the engine cannot see. This module is for *running* theories and
analysing what they did. To write one, see `theories/README.md`.

## Setup

Build the `full` preset first:

```powershell
./scripts/dev-env.ps1 cmake --preset full
./scripts/dev-env.ps1 cmake --build --preset full
```
"""
    ),
    code(
        """import math
import sys
from pathlib import Path

REPO = Path.cwd()
while not (REPO / 'vcpkg.json').exists() and REPO != REPO.parent:
    REPO = REPO.parent

MODULE_DIR = next(
    (p for p in sorted(REPO.glob('out/build/*/bin')) if any(p.glob('marketscan*.pyd'))),
    None,
)
assert MODULE_DIR is not None, 'build the full preset first'
sys.path.insert(0, str(MODULE_DIR))

import marketscan as ms

print(f'marketscan {ms.__version__}  ABI {ms.ABI_VERSION}  {ms.BUILD_TYPE}')
print(f'built by   {ms.COMPILER}')
"""
    ),
    md(
        """## Run a backtest

`ms.run` discovers every theory in the folders it is given, replays the data
through all of them at once, and hands back what happened. Each theory sees the
same events in the same order and owns its own account; none can see any other.
"""
    ),
    code(
        """DATA = REPO / 'data' / 'samples' / 'synthetic_spy_1min.csv'
THEORIES = MODULE_DIR / 'theories'

result = ms.run(theories=[str(THEORIES)], data=str(DATA), cash=100_000)

print(f'{result.events:,} events over {len(result.symbols)} symbol(s): {result.symbols}')
print(f'fill model: {result.fill_model}')
print()
for plugin in result.plugins:
    state = str(plugin.state).split('.')[-1].lower()
    line = f'  {plugin.id:<16} {state:<12}'
    if plugin.error:
        line += plugin.error
    print(line)
"""
    ),
    md(
        """## The leaderboard

Statistics that cannot be computed come back as `nan`, never as zero. A Sharpe
ratio of zero means *no risk-adjusted edge*, which is a specific claim; a series
too short to have one is not making that claim. Check with `math.isfinite`.
"""
    ),
    code(
        """def fmt(value, precision=2):
    return f'{value:.{precision}f}' if math.isfinite(value) else '--'

header = (f"{'theory':<18}{'equity':>13}{'return':>9}{'sharpe':>9}"
          f"{'max dd':>9}{'trades':>8}{'slippage':>11}")
print(header)
print('-' * len(header))

for t in result.theories:
    s = t.summary
    print(f'{t.display_name:<18}'
          f'{t.final_equity.dollars:>13,.2f}'
          f'{s.total_return * 100:>8.2f}%'
          f'{fmt(s.sharpe):>9}'
          f'{s.max_drawdown * 100:>8.2f}%'
          f'{t.trades:>8}'
          f'{t.slippage.dollars:>11,.2f}')
"""
    ),
    md(
        """## What the fill assumption is worth

This is the number the execution simulator exists to produce.

The default model crosses the spread: a market buy pays the ask and a market
sell receives the bid, always. The optimistic model fills everything at the last
trade price, instantly, in full, with no spread and no penalty for size -- which
is what a naive backtest does without saying so.

Run the same data through both and subtract. The difference is profit that was
never available.
"""
    ),
    code(
        """wishful = ms.run(theories=[str(THEORIES)], data=str(DATA), cash=100_000,
                 optimistic_fills=True)

for honest in result.theories:
    other = wishful.find(honest.id)
    gap = other.final_equity - honest.final_equity
    print(f'{honest.display_name}:')
    print(f'  realistic fills   {honest.final_equity.dollars:>12,.2f}')
    print(f'  optimistic fills  {other.final_equity.dollars:>12,.2f}')
    print(f'  imaginary profit  {gap.dollars:>12,.2f}   on {honest.trades} trade(s)')
"""
    ),
    md(
        """## Cash is exact

Binary floating point cannot represent `0.01`. A `double` ledger drifts, and the
drift looks exactly like a small trading edge. Every cash quantity here is an
integer count of micro-dollars.
"""
    ),
    code(
        """total = ms.Money.from_dollars(0)
cent = ms.Money.from_double(0.01)
for _ in range(1_000_000):
    total = total + cent

naive = 0.0
for _ in range(1_000_000):
    naive += 0.01

print(f'exact:  {total}')
print(f'double: {naive!r}')
print(f'equal to 10,000?  {naive == 10_000.0}')
"""
    ),
    md(
        """## The blotter

Slippage has its own column because separating *the signal was wrong* from *I
could not get filled* is the point of simulating execution. It is measured
against the mid the strategy could see when it decided.
"""
    ),
    code(
        """theory = result.theories[0]

print(f"{'time':<21}{'side':<6}{'qty':>8}{'signal':>11}{'fill':>11}{'slippage':>11}")
for trade in theory.blotter[:10]:
    when = ms.time.to_iso8601(trade.ts)[:19].replace('T', ' ')
    side = 'BUY' if trade.side == ms.Side.BUY else 'SELL'
    print(f'{when:<21}{side:<6}{trade.quantity:>8.0f}'
          f'{trade.reference_price:>11.4f}{trade.fill_price:>11.4f}'
          f'{trade.slippage.dollars:>11,.2f}')
"""
    ),
    md(
        """## Metrics on your own numbers

Everything the leaderboard uses is a plain function over a sequence, so a
hypothesis can be checked without going through the engine at all.
"""
    ),
    code(
        """returns = theory.returns
ppy = ms.metrics.infer_periods_per_year(ms.time.MINUTE)

print(f'observations       {len(returns):,}')
print(f'periods per year   {ppy:,.0f}')
print()
print(f'sharpe             {fmt(ms.metrics.sharpe(returns, ppy))}')
print(f'sortino            {fmt(ms.metrics.sortino(returns, ppy))}')
print(f'volatility         {fmt(ms.metrics.volatility(returns, ppy) * 100)}%')

dd = ms.metrics.max_drawdown(theory.equity_curve)
print()
print(f'max drawdown       {dd.max_drawdown * 100:.2f}%')
print(f'  peak at index    {dd.peak_index}')
print(f'  trough at index  {dd.trough_index}')
print(f'  recovered        {dd.recovered}')
"""
    ),
    md(
        """### Undefined is not zero

A series that never moved has no risk-adjusted return. Reporting infinity would
put a strategy that did nothing at the top of the leaderboard; reporting zero
would make it indistinguishable from one that tried and broke even.
"""
    ),
    code(
        """value = ms.metrics.sharpe([0.01] * 100)
print(f'sharpe of a constant series:    {value}  (defined: {ms.metrics.is_defined(value)})')
print(f'correlation with a flat series: {ms.metrics.correlation([1, 2, 3, 4], [7, 7, 7, 7])}')
print(f'calmar with no drawdown:        {ms.metrics.calmar(0.2, 0.0)}')
"""
    ),
    md(
        """## Plots

Optional -- everything above works without matplotlib.
"""
    ),
    code(
        """try:
    # No backend is forced: inside a kernel matplotlib uses the inline backend,
    # which is what puts the figure in the notebook. Selecting Agg here would
    # make plt.show() a silent no-op and the committed notebook would have a
    # plotting cell with nothing under it.
    import matplotlib.pyplot as plt
except ImportError:
    print('matplotlib not installed; skipping the charts')
else:
    fig, (top, bottom) = plt.subplots(
        2, 1, figsize=(11, 7), sharex=True, gridspec_kw={'height_ratios': [2, 1]})

    for t in result.theories:
        top.plot(t.times, t.equity_curve, linewidth=1.6, label=t.display_name)
        underwater = [d * 100 for d in t.drawdown]
        bottom.fill_between(t.times, underwater, 0, alpha=0.3)
        bottom.plot(t.times, underwater, linewidth=1.2)

    top.set_ylabel('Equity')
    top.set_title('MarketScan -- realistic fills')
    top.legend(loc='best')
    top.grid(alpha=0.25)

    bottom.set_ylabel('Drawdown (%)')
    bottom.set_xlabel('Seconds since the Unix epoch')
    bottom.grid(alpha=0.25)

    fig.tight_layout()
    plt.show()
"""
    ),
    md(
        """---

## Where to go next

- `theories/README.md` -- writing a theory. Two headers and one macro, and it
  builds without the rest of this project present.
- `marketscan.exe` -- the desktop dashboard: twelve panels, a transport with a
  scrub bar, and hot reload from the same `theories/` folder.
- `marketscan.exe --headless --compare-fills` -- the table above from the
  command line, through the same `run_backtest` this notebook calls.

Neither reference theory does anything a reasonable person would call a
strategy, and that is the point. The car is finished; the engine bay is empty
and every mount is standard.
"""
    ),
]


def main() -> int:
    notebook = {
        "cells": CELLS,
        "metadata": {
            "kernelspec": {
                "display_name": "Python 3",
                "language": "python",
                "name": "python3",
            },
            "language_info": {"name": "python", "version": "3.12"},
        },
        "nbformat": 4,
        "nbformat_minor": 5,
    }

    TARGET.parent.mkdir(parents=True, exist_ok=True)
    with TARGET.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(notebook, handle, indent=1)
        handle.write("\n")

    print(f"wrote {TARGET.relative_to(REPO)} ({len(CELLS)} cells)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
