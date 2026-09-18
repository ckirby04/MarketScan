"""The M7 report: turns the measurements into `docs/PATHOLOGIES.md`.

Separate from `pathology.py` so that the measuring and the writing-up are separate
concerns -- a change to the prose cannot change a number, and the measurement
module has no opinions about markdown.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np

from research.pathology import HONEST_R2_CEILING, SURVIVORSHIP_SPEC, Finding

REPO = Path(__file__).resolve().parent.parent
REPORT = REPO / "docs" / "PATHOLOGIES.md"


def _table(rows: list[tuple[str, str]]) -> list[str]:
    return ["| | |", "|---|---|"] + [f"| {left} | {right} |" for left, right in rows]


def _t_text(value: float) -> str:
    """A t-statistic, printed as an order of magnitude once it stops meaning much.

    A feature that is the label ranks the cross-section perfectly or nearly so on
    every date, and the information coefficient that results varies only at the
    fifth decimal. The t-statistic is then arithmetically correct and hundreds of
    thousands large, which reads as a bug rather than as the tell it is. Above ten
    thousand the magnitude is the only informative part of it.
    """
    if not np.isfinite(value):
        return "--"
    if abs(value) > 1e4:
        return f"~1e{int(np.floor(np.log10(abs(value))))}"
    return f"{value:+.1f}"


def write_report(findings: dict[str, Finding], details: dict[str, dict[str, float]],
                 path: Path = REPORT) -> Path:
    """Writes the report from the measurements just taken. No numbers of its own."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)

    survivorship = findings["survivorship"]
    lookahead = findings["lookahead"]
    delisting = findings["delisting"]
    s = details["survivorship"]
    k = details["lookahead"]
    d = details["delisting"]

    lines = [
        "# Pathologies",
        "",
        "Three biases injected on purpose, and what each one was worth on a panel whose",
        "true signal strength is known. Regenerate with `python -m research.pathology`;",
        "the assertions live in `research/tests/test_pathologies.py`.",
        "",
        "**Every number here is a statement about this pipeline on synthetic data.** The",
        "magnitudes depend on the departure rate, the hazard tilt and the panel length,",
        "all of which were chosen to make an effect measurable rather than to match any",
        "real index. What transfers is the direction, the mechanism, and the order of",
        "magnitude -- not the decimals.",
        "",
        "| | honest | with the bias | ratio |",
        "|---|---|---|---|",
    ]
    for finding in (survivorship, lookahead, delisting):
        ratio = finding.inflation
        ratio_text = f"{ratio:.2f}x" if np.isfinite(ratio) else "--"
        lines.append(f"| {finding.name} ({finding.units}) | {finding.honest:+.6f} | "
                     f"{finding.pathological:+.6f} | {ratio_text} |")

    lines += [
        "",
        "---",
        "",
        "## 1. Survivorship",
        "",
        f"Panel: {int(s['names_point_in_time'])} securities over "
        f"{SURVIVORSHIP_SPEC.n_periods} periods, planted lambda "
        f"{SURVIVORSHIP_SPEC.lam:+.6f}, departure hazard "
        f"{SURVIVORSHIP_SPEC.delist_hazard:g} per period tilted towards *high* exposure,",
        f"departure return {SURVIVORSHIP_SPEC.delist_return:.0%}. "
        f"{int(s['departures'])} names left. A survivors-only universe keeps "
        f"{int(s['names_survivors_only'])} of them.",
        "",
        "### The index level",
        "",
    ] + _table([
        ("equal-weight return, point-in-time",
         f"{s['benchmark_point_in_time']:+.2%} a year"),
        ("equal-weight return, survivors only",
         f"{s['benchmark_survivors_only']:+.2%} a year"),
        ("**difference**",
         f"**{s['benchmark_survivors_only'] - s['benchmark_point_in_time']:+.2%} a year**"),
    ]) + [
        "",
        "This is the part of survivorship bias whose sign is not negotiable. The survivors",
        "are the names that did not fall over; an index built from them returns more than",
        "the one anybody could have held, every time.",
        "",
        "### The premium",
        "",
    ] + _table([
        ("lambda, point-in-time", f"{survivorship.honest:+.6f}"),
        ("lambda, survivors only", f"{survivorship.pathological:+.6f}"),
        ("inflation", f"**{survivorship.inflation:.2f}x**"),
        ("standard error of either estimate", f"{s['stderr']:.6f}"),
        ("information coefficient",
         f"{s['ic_point_in_time']:+.4f} -> {s['ic_survivors_only']:+.4f}"),
    ]) + [
        "",
        "The mechanism: the hazard here is tilted towards names with a *high* exposure --",
        "the ones a long-short book following this signal is long. Their exits are losses",
        "the book took. Remove the names and the losses go with them, and what is left is",
        "a signal whose failures have been deleted from the record.",
        "",
        "### The sign is not a property of survivorship",
        "",
        "The identical experiment with the hazard tilted the other way, so that the",
        "failures are among the names the book is *short*:",
        "",
    ] + _table([
        ("lambda, point-in-time", f"{s['mirrored_honest']:+.6f}"),
        ("lambda, survivors only", f"{s['mirrored_biased']:+.6f}"),
        ("ratio", f"**{s['mirrored_biased'] / s['mirrored_honest']:.2f}x**"),
    ]) + [
        "",
        "The bias *deflates* the premium. Being short a name that fell was a gain, and a",
        "survivors-only universe removes it.",
        "",
        "This milestone's first run was configured that way and reported survivorship bias",
        "shrinking the premium -- the opposite of what everybody expects to read. It was",
        "not a bug, and tuning until the expected sign appeared would have been the actual",
        "mistake. **Survivorship bias moves a measured premium in whichever direction the",
        "failures were correlated with the signal.** The size is the reliable part, and in",
        "both directions here it is tens of percent of the premium being measured.",
        "",
        "**The guard.** The engine gives a strategy no way to see a non-member",
        "(`docs/IDENTITY.md`), and `Panel.frame` holds no row for a security on a date it",
        "was not a member. Survivorship has to be *added* by a deliberate step --",
        "`research.pathology.survivors_only` -- which is what this measurement does.",
        "",
        "## 2. Lookahead",
        "",
        "### The gross case: tomorrow's return as today's feature",
        "",
    ] + _table([
        ("out-of-sample R-squared, honest", f"{lookahead.honest:+.5f}"),
        ("out-of-sample R-squared, leaking", f"**{lookahead.pathological:+.5f}**"),
        ("information coefficient", f"{k['honest_ic']:+.4f} -> {k['leaked_ic']:+.4f}"),
        ("its t-statistic", f"{_t_text(k['honest_ic_t'])} -> {_t_text(k['leaked_ic_t'])}"),
    ]) + [
        "",
        "The leaked information coefficient is **1.0 to four decimal places**, on every",
        "single date -- a perfect ranking of the cross-section, over and over. Its only",
        "variation is at the fifth decimal, where the model blends the leak with the",
        "honest feature, so the t-statistic that falls out is six digits long. That number",
        "is arithmetically correct and tells you nothing except that the question was",
        "wrong: a statistic whose sampling variation is rounding error is not evidence",
        "about a population, and a t-statistic of a hundred thousand has never once in the",
        "history of finance meant what it appears to mean.",
        "",
        f"**This is the smell.** An out-of-sample R-squared above {HONEST_R2_CEILING:.0%} on",
        "daily cross-sectional returns is not a discovery, it is a leak. No honest daily",
        "feature reaches it: a number that size implies an information coefficient no",
        "published factor sustains. If a result ever looks like this, the first hypothesis",
        "is a leak. The second hypothesis is also a leak.",
        "",
        "### The realistic case: a feature window that overlaps the label",
        "",
        "A ten-period forward label, and a feature that has seen the fifth of those ten",
        "periods. Nobody writes this on purpose; it is what an off-by-a-window arithmetic",
        "error produces.",
        "",
    ] + _table([
        ("out-of-sample R-squared, honest", f"{k['partial_honest_r2']:+.5f}"),
        ("out-of-sample R-squared, overlapping", f"{k['partial_leaked_r2']:+.5f}"),
        ("information coefficient",
         f"{k['partial_honest_ic']:+.4f} -> {k['partial_leaked_ic']:+.4f}"),
    ]) + [
        "",
        "Smaller, and much easier to believe. This is the one that gets published, and it",
        "is why `splits.py` purges on **label windows** rather than on dates: the overlap",
        "that matters is between what a label is made of and what a feature has already",
        "seen, and neither of those is visible from a date alone.",
        "",
        "**The guard.** Every transform in `research/features.py` is causal, and",
        "`test_features.py` proves it structurally rather than transform by transform: it",
        "perturbs the final date and asserts no feature value on any earlier date moves. A",
        "new transform is covered by that test the moment it is registered. The leak above",
        "had to be requested by name.",
        "",
        "## 3. Delisting",
        "",
        f"{int(d['departures'])} names taken to **-100%** and removed from the universe. A",
        "ten-period forward label has no ten-period window for a name with four periods",
        "left, so a pipeline has to decide what that label is. The two policies in",
        "`labels.py`, on the same panel:",
        "",
    ] + _table([
        ("labels lost by dropping truncated windows", f"{int(d['labels_dropped'])}"),
        ("cumulative return carried by those labels", f"{d['truncated_label_return']:+.1f}"),
        ("mean forward return in the study, compounded", f"{d['mean_label_compound']:+.6f}"),
        ("mean forward return in the study, dropped", f"**{d['mean_label_drop']:+.6f}**"),
        ("labels that swallowed a departure whole, compounded",
         f"{int(d['ruinous_labels_compound'])}"),
        ("labels that swallowed a departure whole, dropped",
         f"**{int(d['ruinous_labels_drop'])}**"),
        ("worst single label, either policy", f"{d['worst_label_compound']:+.4f}"),
        ("positions force-exited by the book", f"{int(d['forced_exits'])}"),
        ("return earned in departure periods",
         f"{d['portfolio_delisting_return_compound']:+.5f}"),
    ]) + [
        "",
        "The average forward return in the study goes from "
        f"{d['mean_label_compound']:+.2%} to {d['mean_label_drop']:+.2%} -- the naive policy",
        f"removes {1.0 - d['mean_label_drop'] / d['mean_label_compound']:.0%} of the average",
        "loss. Those rows are not adjusted for, or modelled around, or argued about: they",
        "are *absent*. Nobody decided to exclude them. The label window had nowhere to end",
        "and the row went away.",
        "",
        "**And the worst case survives, which is what makes it dangerous.** Of the ten",
        "labels whose window contains a given departure, exactly one is untruncated -- the",
        "window that ends precisely on the departure date -- and it survives both policies.",
        f"So {int(d['ruinous_labels_compound'])} ruinous labels become "
        f"{int(d['ruinous_labels_drop'])}, but the minimum of the sample is unchanged at "
        f"{d['worst_label_compound']:+.2f}.",
        "",
        "A study that sanity-checks its own worst observation therefore finds it present,",
        "correct, and catastrophic, and concludes the delistings are being counted. They",
        "are being counted once each instead of ten times each.",
        "",
        "(That paragraph is here because the first draft of this report asserted the",
        "opposite -- that the surviving rows were the ones further back -- and the",
        "assertion failed against the measurement. The counts above are now counted rather",
        "than argued.)",
        "",
        f"The estimated lambda moves too, from {d['lambda_compound']:+.6f} to "
        f"{d['lambda_drop']:+.6f} against a standard error of {d['lambda_stderr']:.6f}.",
        "That one is reported and not leaned on: the hazard in this panel is untilted, so",
        "what a -100% label does to a cross-sectional *slope* depends on the exposure the",
        "dying name happened to have, and across one panel that is mostly the seed. The",
        "level effect above is the systematic one.",
        "",
        "**The portfolio side holds independently.** A book long a name that goes to -100%",
        "earns -100% on that position, because the departure return is the final period of",
        "that name's return series and the position is still on when it lands. The loss is",
        "in `PortfolioResult.active_returns`, and `delisting_loss` reports it separately so",
        "it cannot be mistaken for anything else.",
        "",
        "**What is not measured here.** The engine's half -- force-liquidation at the last",
        "price, logged distinctly in the blotter, proceeds credited to cash -- is asserted",
        "by `tests/test_universe.cpp` (\"a held security that leaves the universe is",
        "liquidated\") and is joined to this pipeline at M9, where the same strategy runs",
        "both ways and the two have to agree. Nothing in this file has run the C++ ledger.",
        "",
        "And the engine's forced exit is **optimistic**: it liquidates at the last",
        "available price, where a real delisting return is frequently far worse. That is a",
        "known limitation with its own entry in `docs/STATUS.md`, and nothing in this",
        "milestone fixes it. What this milestone establishes is what it would be worth if",
        "it were wrong.",
        "",
        "---",
        "",
        "## What this milestone is for",
        "",
        "Not to prove the guards exist -- the tests do that. To put a number on what each",
        "bias is worth, so that a future result which happens to look good can be checked",
        "against the size of the mistakes that produce results which look good.",
        "",
    ]

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path
