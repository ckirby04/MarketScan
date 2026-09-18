"""Assembles `docs/FALSE_DISCOVERY.md` from the sweep's measurements."""

from __future__ import annotations

import json
from pathlib import Path

import pandas as pd

from research.sweep_report_body import (
    caveats,
    champion,
    experiment,
    ic_calibration,
    remedy,
    single_variant,
    the_search,
)

REPO = Path(__file__).resolve().parent.parent
REPORT = REPO / "docs" / "FALSE_DISCOVERY.md"
IC_RESULTS = REPO / "docs" / "figures" / "ic_calibration.json"
SCALING_RESULTS = REPO / "docs" / "figures" / "sample_length.json"


def _load(path: Path) -> dict | None:
    """A measurement if it has been taken, None if it has not.

    The report is assembled from whatever measurements exist rather than
    requiring all of them, so that regenerating it after one sweep does not
    silently drop the sections belonging to another.
    """
    if not Path(path).exists():
        return None
    return json.loads(Path(path).read_text(encoding="utf-8"))["summary"]


def verdict(threshold: float) -> list[str]:
    """What the measured threshold implies, said plainly.

    Spec 2 §9 asks for this to be loud if the number invalidates the approach and
    quietly honest if it does not. The bands are judgment calls, written here so
    the judgment is visible rather than carried by tone.
    """
    if threshold >= 1.75:
        return [
            "### This is a problem, and it should be said plainly",
            "",
            f"A bar of {threshold:.2f} is higher than most real strategies ever reach, and it",
            "was produced by three years of data and a fortnight's worth of variants. A",
            "search of that size over a sample that short **cannot distinguish a genuine",
            "signal from the best of its own noise** unless the signal is extraordinary.",
            "",
            "That is not an argument for a better model. It is an argument about the",
            "experiment: fewer variants, longer samples, and the threshold computed and",
            "written down *before* the result is admired. The bar falls fast as the search",
            "narrows -- the best-of-N table above is the map -- and a real signal then has",
            "somewhere to show up.",
            "",
            "Anything this project reports from a search of this shape, on data of this",
            "length, below this Sharpe, is not evidence. Including anything it reports",
            "later, on real data, having forgotten this page.",
        ]
    if threshold >= 1.0:
        return [
            "### What this means for anything measured later",
            "",
            f"A Sharpe below {threshold:.2f} from a search of this size is not evidence. It is",
            "inside the range a search over data containing nothing produces as a matter of",
            "course. That does not make it false; it makes it indistinguishable from the",
            "alternative, which is the same thing as far as a decision goes.",
            "",
            "The bar moves with the number of variants tried, so the honest practice is to",
            "count them -- including the ones abandoned after a glance -- and judge the",
            "survivor against the threshold for that count rather than against zero.",
        ]
    return [
        "### What this means for anything measured later",
        "",
        f"A threshold of {threshold:.2f} is low enough that a real signal of ordinary size",
        "would clear it. That is a comfortable result and worth being suspicious of: it",
        "depends on the sample length, the number of variants and the absence of trading",
        "costs, and every one of those moves it upwards in a real study.",
    ]


def write_report(frame: pd.DataFrame, summary: dict[str, object],
                 ic: dict[str, object] | None = None,
                 scaling: dict[str, object] | None = None,
                 path: Path = REPORT) -> Path:
    """Assembles the document. `ic` and `scaling` default to the committed runs."""
    ic = ic if ic is not None else _load(IC_RESULTS)
    scaling = scaling if scaling is not None else _load(SCALING_RESULTS)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)

    threshold = float(summary["threshold"])
    variants = int(summary["variants"])

    lines = [
        "# False discovery",
        "",
        "What this pipeline finds in data that contains nothing at all.",
        "",
        "Regenerate with `python -m research.sweep`. The threshold is importable as",
        "`research.thresholds.NULL_SHARPE_THRESHOLD`, and `research/tests/test_sweep.py`",
        "asserts that the constant and this measurement still agree -- so a future result",
        "is judged against a measured bar rather than a remembered one.",
        "",
        "> **The bar, for a search of this size on data of this length:**",
        f"> an annualized Sharpe of **{threshold:.2f}**.",
        "",
    ]
    lines += experiment(summary, variants)
    lines += single_variant(summary)
    lines += the_search(summary, variants)
    lines += champion(frame)
    lines += ["## The threshold", ""]
    lines += [
        f"> **A Sharpe of {threshold:.2f}.** A search of {variants} variants over three years",
        "> of this data produces a best result at or above it one time in twenty **when",
        "> nothing is there at all**.",
        "",
    ]
    lines += verdict(threshold)
    lines += [""]
    if ic is not None:
        lines += ic_calibration(ic)
    if scaling is not None:
        lines += remedy(scaling)
    lines += caveats(summary, variants)

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path
