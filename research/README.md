# The research layer

A dynamometer for the backtester. It generates market panels with a **known**
signal, runs a full research pipeline over them, and measures what comes back —
first against a signal that was planted, then against biases injected on purpose,
then against no signal at all.

> **Nothing in here says anything about markets.** Every panel is synthetic, every
> number is a statement about this pipeline, and the portfolio layer charges no
> commission and crosses no spread. Results that look good here look good because
> the data was built that way.

Spec 2, milestones M4–M8. Pure Python — pandas, numpy, scipy, scikit-learn — and
it does **not** run through the C++ event engine. Factor research is a
cross-sectional panel problem; forcing it through an event loop would be slow and
miserable. The two meet at M9, where the same strategy is written both ways and the
results have to agree.

---

## What it found

| | | |
|---|---|---|
| **M6** | The pipeline recovers a planted signal across six values of λ, 18 runs, all within two standard errors. | [`docs/figures/lambda_recovery.png`](../docs/figures/lambda_recovery.png) |
| **M7** | Survivorship, lookahead and delisting injected deliberately, each with the magnitude it was worth. | [`docs/PATHOLOGIES.md`](../docs/PATHOLOGIES.md) |
| **M8** | The Sharpe a search of this size produces on data containing **nothing**. | [`docs/FALSE_DISCOVERY.md`](../docs/FALSE_DISCOVERY.md) |
| **M8b** | The same treatment for the information coefficient, whose t-statistic turned out to fire 8–26% of the time on nothing. | [`docs/figures/ic_calibration.png`](../docs/figures/ic_calibration.png) |

M8 is the one that matters. Every result this project ever produces is worth
exactly as much as that number, and `research/thresholds.py` exposes both bars as
constants so a future result is judged against a measured bar rather than a
remembered one.

**Nothing in here is judged against a textbook threshold.** A t-statistic of 1.96
means one thing in a statistics course and another in a pipeline with overlapping
labels and persistent features; the difference was measured rather than argued
about, and the measurement is what the code imports.

---

## Running it

```powershell
pytest                                    # the whole suite
python -m research.recovery               # M6: regenerates the recovery figure
python -m research.pathology              # M7: regenerates docs/PATHOLOGIES.md
python -m research.sweep                  # M8: 1512 runs, the better part of an hour
python -m research.sweep --quick          # the machinery, in a minute
python -m research.export --out data/cache/synthetic   # a panel the engine can replay
```

Requirements are in [`requirements.txt`](requirements.txt). Nothing here is
installed as a package: run it from the repository root.

---

## The modules

| | |
|---|---|
| `panel.py` | **M4.** Generates a panel: `r = β·m + λ·x + ε`, seeded, with churn and delistings. The exposure is lagged one period against the return it explains, which is the difference between an honest pipeline and the lookahead pathology. |
| `export.py` | Writes a panel as daily bars and a membership history in the formats the C++ side already reads, so the synthetic panel enters the engine through the same door real data does. |
| `features.py` | **M5.** Causal panel transforms. Every feature on date *t* is a function of data at or before *t*, proved structurally by a test that perturbs tomorrow and asserts today does not move. The one transform that can see the future is called `leak`. |
| `labels.py` | Forward returns, and the question of what a dead name's label is. Two policies; the difference between them is a measurement in M7. |
| `splits.py` | Purged and embargoed k-fold. Folds are contiguous date blocks; training rows whose label windows overlap the fold are dropped, then an embargo on top. It reports what that cost. |
| `models.py` | A ridge, a GBM, and the mean. The panel is linear by construction, so the ridge is correctly specified and the GBM is the over-flexible one — they are here for contrast, not competition. |
| `portfolio.py` | Active weights with sector neutrality, per-name caps, a beta target and a turnover budget, found by projection and **validated against its own constraints** before being returned. |
| `pipeline.py` | The lot, end to end, with every number carrying its caveat. |
| `metrics.py` | Statistics, and the Fama–MacBeth estimator λ is measured with. Undefined results are NaN, never zero — the same rule the C++ metrics follow. |
| `recovery.py` | **M6.** |
| `pathology.py`, `pathology_report.py` | **M7.** |
| `sweep.py`, `sweep_report.py`, `sweep_report_body.py` | **M8.** |
| `ic_calibration.py` | **M8b.** The information coefficient bar, per horizon. |
| `sample_length.py` | What actually moves the bar: sample length against variant discipline. |
| `thresholds.py` | The measured bars, importable. |

---

## Two things that are easy to get wrong, and how they are handled here

**The lag.** `x` on date *t* is what an observer knows at the close of *t*, and the
return it explains is the one realized over *t+1*. Spec 2 writes the model with a
single time index; written that way literally, the exposure would be contemporaneous
with the return it predicts, which is lookahead. `panel.py` lags it and `labels.py`
labels forward, and M7 injects the alternative on purpose to see what it is worth.

**The estimate of λ is measured against the raw exposure, not the standardized
feature built from it.** A per-date cross-sectional z-score divides by a number
that fluctuates with the cross-section, so a recovery plot built on the feature
would have slightly different units on its two axes and would be measuring the
normalization. What the *model* contributed is reported separately: out-of-sample
R², information coefficient, calibration slope, and the portfolio's Sharpe.

---

## What is not here

- **No real factor.** No momentum, no value, no quality. The synthetic exposure is
  the only signal this phase consumes; a named factor would invite the reading that
  recovering it said something about markets.
- **No trading costs.** The engine owns execution and crosses the spread by
  default — and also charges market impact, which on orders of a few hundred
  shares against a hundred displayed is an order of magnitude the larger of the
  two. A second, approximate cost model here would be a second answer to a
  question that already has one. `research.parity.modelled_cost` predicts what
  the engine will charge rather than duplicating it.
- **No real delisting return.** `PanelSpec.delist_return` is a planted number and
  the engine assumes the last price it saw. Both are instances of **the assumed
  exit** — see [`docs/EXECUTION.md`](../docs/EXECUTION.md) — and CRSP delisting
  returns replace both in one change.
- **No real data.** `data/cache/` is gitignored, no test touches the network, and
  the committed fixtures are synthetic.
