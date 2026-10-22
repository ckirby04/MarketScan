#!/usr/bin/env python3
"""Builds the catalog's synthetic recipes, with validation measured from the data.

    python scripts/build_recipes.py
    python scripts/build_recipes.py --daily ../marketscan-data/sharadar ../marketscan-data/fred \\
        --out ../marketscan-data/recipes.json

A recipe (CATALOG.md, "Synthetic recipe") simulates a fund before its first real
bar from an underlying. Its parameters are written here, each with where it came
from; its validation is never written anywhere by hand. This script computes it
with research/synthetic.py, over the years where the real fund and the
simulation both exist, and writes the records -- outside the repository, beside
the data they were measured on, since they are measurements of licensed data.

A recipe whose inputs are missing is reported and left out, never written
without its validation: CATALOG.md says a recipe with no validation cannot be
used, and the engine refuses one.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from research import synthetic  # noqa: E402

DATA = REPO.parent / "marketscan-data"

# Each parameter with its source. borrow_spread_pct is the value the form
# contract's example carried before any validation was run, kept so that the
# validation tests a parameter rather than fits one.
RECIPES = [
    {
        "recipe_id": "tqqq_from_qqq_3x",
        "target": "TQQQ",
        "underlying": "QQQ",
        "params": {
            "leverage": 3,
            "annual_fee_pct": 0.95,
            "fee_source": "ProShares UltraPro QQQ summary prospectus, 2010-10-01 (SEC form 497K): "
                          "total annual operating expenses after waivers 0.95%",
            "borrow_spread_pct": 0.5,
            "cash_rate_series_id": "dtb3",
            "underlying_adjustment": "total_return",
        },
    },
]


def load_reader():
    spec = importlib.util.spec_from_file_location("marketscan_fetch", REPO / "scripts" / "fetch_stooq.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.read_msb


def find(directories: list[Path], name: str) -> Path | None:
    for directory in directories:
        path = directory / f"{name}.msb"
        if path.exists():
            return path
    return None


def revision() -> str:
    try:
        out = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def build(spec: dict, directories: list[Path], read_msb, computed_by: str) -> dict:
    p = spec["params"]
    underlying_name = spec["underlying"] + (".TR" if p.get("underlying_adjustment") == "total_return" else "")
    paths = {
        "target": find(directories, spec["target"]),
        "underlying": find(directories, underlying_name),
        "rate": find(directories, p["cash_rate_series_id"].upper()),
    }
    missing = [f"{role} ({name})" for (role, path), name in zip(
        paths.items(), [spec["target"], underlying_name, p["cash_rate_series_id"].upper()]) if path is None]
    if missing:
        raise synthetic.RecipeError("no data for " + ", ".join(missing))

    def columns(path):
        _ticker, rows = read_msb(path)
        return [d for d, _ in rows], [c for _, c in rows]

    target_days, target = columns(paths["target"])
    underlying_days, underlying = columns(paths["underlying"])
    rate_days, rates = columns(paths["rate"])
    params = synthetic.Params(p["leverage"], p["annual_fee_pct"], p["borrow_spread_pct"])
    validation = synthetic.validate(underlying_days, underlying, rate_days, rates,
                                    target_days, target, params)
    # Building the history itself proves the recipe can produce it: an
    # underlying missing the target's first day, or a wipeout, refuses here.
    days, _closes = synthetic.extend(underlying_days, underlying, rate_days, rates,
                                     target_days[0], target[0], params)
    return {
        "record": "synthetic_recipe",
        "recipe_id": spec["recipe_id"],
        "target_security_id": spec["target"].lower(),
        "underlying_security_id": spec["underlying"].lower(),
        "method": "daily_leverage",
        "params": p,
        "available_from": days[0] if days else target_days[0],
        "validation": {**validation, "computed_by": computed_by},
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--daily", nargs="+", type=Path, default=[DATA / "sharadar", DATA / "fred"])
    parser.add_argument("--out", type=Path, default=DATA / "recipes.json")
    args = parser.parse_args(argv)
    out = args.out.resolve()
    if out == REPO or REPO in out.parents:
        print(f"{args.out} is inside the repository; recipes are measured on licensed data and live "
              "beside it (docs/DATA_POLICY.md section 4)", file=sys.stderr)
        return 2

    read_msb = load_reader()
    computed_by = f"scripts/build_recipes.py at {revision()}"
    records, failures = [], 0
    for spec in RECIPES:
        try:
            record = build(spec, args.daily, read_msb, computed_by)
        except synthetic.RecipeError as error:
            print(f"{spec['recipe_id']}: not built: {error}", file=sys.stderr)
            failures += 1
            continue
        records.append(record)
        v = record["validation"]
        print(f"{record['recipe_id']}: from {record['available_from']}; over {v['overlap_from']} to "
              f"{v['overlap_to']}: correlation {v['daily_return_correlation']:.4f}, tracking "
              f"{v['annualized_tracking_difference_pct']:+.2f}% a year, largest gap "
              f"{v['max_cumulative_gap_pct']:.1f}%")
    out.write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
