"""A one-minute check that the Python module works.

Run it after building the `full` preset:

    python bindings/python/examples/smoke_test.py

It imports the module, runs a backtest over the committed sample dataset, and
prints what came back. If this works, the notebook will.
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def find_module_dir() -> Path:
    """Finds the build output holding marketscan.*.pyd."""
    candidates = sorted(REPO.glob("out/build/*/bin"))
    for candidate in candidates:
        if any(candidate.glob("marketscan*.pyd")):
            return candidate
    raise SystemExit(
        "Could not find the built module. Build the 'full' preset first:\n"
        "  ./scripts/dev-env.ps1 cmake --preset full\n"
        "  ./scripts/dev-env.ps1 cmake --build --preset full"
    )


def main() -> int:
    module_dir = find_module_dir()
    sys.path.insert(0, str(module_dir))

    import marketscan as ms

    print(f"marketscan {ms.__version__}  ABI {ms.ABI_VERSION}  {ms.BUILD_TYPE}")
    print(f"module: {module_dir}")
    print()

    result = ms.run(
        theories=[str(module_dir / "theories")],
        data=str(REPO / "data" / "samples" / "synthetic_spy_1min.csv"),
        cash=100_000,
    )

    if not result.ok:
        print(f"FAILED: {result.error}")
        return 1

    print(f"{result.events} events, {len(result.theories)} theories, "
          f"{result.fill_model} fills")
    print()

    for theory in result.theories:
        summary = theory.summary
        print(f"{theory.display_name}")
        print(f"  equity     {theory.final_equity}")
        print(f"  return     {summary.total_return * 100:+.2f}%")
        print(f"  sharpe     {summary.sharpe:.2f}"
              if math.isfinite(summary.sharpe) else "  sharpe     --")
        print(f"  max dd     {summary.max_drawdown * 100:.2f}%")
        print(f"  trades     {theory.trades}")
        print(f"  slippage   {theory.slippage}")
        print(f"  commission {theory.commission}")
        print(f"  published  {theory.published}")
        print(f"  invariant  {'holds' if theory.invariant_held else 'VIOLATED'}")
        print(f"  curve      {len(theory.equity_curve)} points")

        if not theory.invariant_held:
            print("  *** the ledger created or destroyed value; this is a bug")
            return 1
        print()

    # The metrics are usable on their own, on any sequence of numbers.
    m = ms.metrics
    returns = [0.02, 0.01, 0.03, 0.00]
    print(f"metrics.sharpe({returns}, periods_per_year=1) = "
          f"{m.sharpe(returns, periods_per_year=1.0):.6f}")
    print(f"metrics.max_drawdown([100,120,60,80,130]).max_drawdown = "
          f"{m.max_drawdown([100, 120, 60, 80, 130]).max_drawdown}")

    # Undefined is NaN, never zero.
    flat = m.sharpe([0.01] * 5)
    print(f"metrics.sharpe of a flat series = {flat} "
          f"(defined: {m.is_defined(flat)})")
    assert not m.is_defined(flat), "a riskless series must not have a Sharpe ratio"

    # Cash is exact.
    total = ms.Money.from_dollars(0)
    cent = ms.Money.from_double(0.01)
    for _ in range(1_000_000):
        total = total + cent
    assert total == ms.Money.from_dollars(10_000), "Money drifted"
    print(f"a million cents = {total}  (exact)")

    print("\nOK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
