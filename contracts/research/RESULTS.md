# Results format

What the engine returns for a run, and how the app shows it. Record shape is in
[`results.schema.json`](results.schema.json).

---

## Principle: numbers before words, and the verdict is not written by the LLM

A results screen has four parts, always in this order:

1. **Verdict.** A grade and a few headline numbers.
2. **Reasoning.** The gates behind the grade: what each one measured, its threshold, pass or fail.
3. **Stats.** The full numbers: selected parameters, every window, benchmarks, trades, parameter
   surface, regimes, null comparison, outcome range, data provenance.
4. **Analysis.** The LLM's explanation, labelled as written by AI from the numbers above.

Parts 1 to 3 come from the engine and are rendered by the UI directly from the JSON. The LLM
does not restate them and cannot change them. Analysis, however careful, can mislead, so it
comes last and is accountable to everything above it. If the analysis and the stats ever
disagree, the stats are right by construction.

The verdict is computed by fixed, published rules from the stats (see *Gates*). Two runs with
the same form and data get the same verdict. The rules are versioned, and every result
records the version it was graded under.

---

## The verdict

### Grade

| grade | means |
|---|---|
| `supported` | It beat the benchmark out of sample, beat random timing, survived the multiple-testing correction, sits on a plateau, and never came close to ruin. History supports this rule. |
| `fragile` | It beat the benchmark out of sample, but at least one robustness gate failed. The headline number is probably better than the rule deserves. |
| `not_supported` | It did not beat the benchmark out of sample. |
| `insufficient_evidence` | Too few trades to say anything either way. The most common honest answer for timing rules on a single asset. |

The grade is a statement about history, not a recommendation. The app shows it as
"Historical evidence: supported", not "Good idea".

### Data grade

Shown next to every grade, and not part of it:

- `research`: every series used is `research` tier.
- `exercise_only`: at least one series is not. The UI shows "Unverified data" beside the grade,
  and a `supported` on this data does not count as a finding.

Synthetic history doesn't change the data grade, but if more than half the range is synthetic
the run gets a `MOSTLY_SYNTHETIC` warning, shown under the verdict.

### Headline numbers

Next to the grade:

- out-of-sample CAGR for the strategy and the primary benchmark (the first in `benchmarks`),
- out-of-sample max drawdown for both,
- the **outcome range**: 5th, 50th and 95th percentile CAGR across bootstrap resamples (see below).

### Why there is no "expected ROI"

An expected ROI is a forecast, and a backtest can't produce one. The closest honest number is
the **outcome range**: the market's history is resampled in blocks into many alternative paths
of the same length, the selected rule is re-run on each, and the spread of results is reported.
It answers "if history had been reshuffled, how differently might this have gone?" That
captures how much the result depends on the order events happened in, which is the main thing
a single backtest hides. It still assumes the future resembles the past in distribution, and
the UI says so beside it.

---

## Gates

Criteria version `0.1`. Every Results document records the version it was graded under. The
exact definitions -- each measured quantity, each threshold, the order -- are
[`JOB.md`](JOB.md) section 10, which is what both implementations follow; this table is the
summary. Changing a threshold is a new criteria version.

| id | test | passes when | failing gives |
|---|---|---|---|
| `evidence` | Enough trades to judge | ≥ 5 entries over the full range and ≥ 2 out of sample | `insufficient_evidence` |
| `oos_benchmark` | Beat the benchmark after the optimization window | Out-of-sample objective metric better than the primary benchmark's | `not_supported` |
| `random_timing` | Timing, not just exposure | Objective metric at or above the 95th percentile of the random-timing null | `fragile` |
| `multiple_testing` | Survives having searched | Random-timing p-value, corrected for the effective number of variants tried, below 0.05 | `fragile` |
| `plateau` | Neighbouring parameters also work | Plateau ratio ≥ 0.5 | `fragile` |
| `survival` | No near-ruin | Full-range max drawdown above -90%, and fewer than 5% of bootstrap paths with a drawdown below -90% | `fragile` |

The grade is the first failing gate in table order, or `supported` if all pass. Gates that don't
apply are recorded as `not_applicable` (for example `plateau` and `multiple_testing` when no
parameter is swept), and they neither pass nor fail. A run with no sweep is still a choice
someone made after seeing other ideas, so a run without a sweep carries a
`NO_SEARCH_CORRECTION` warning rather than a clean bill.

Every gate in the result records what it measured, its threshold and the direction of the
comparison, so the UI can print "12 entries (needs ≥ 5)" without the LLM involved.

### Random-timing null

Does the rule's timing add anything over simply being in the market the same amount? For the
selected parameters:

1. Take the rule's actual holding periods.
2. Draw 1,000 alternatives: the same number of entries, the same holding lengths in shuffled
   order, placed at random non-overlapping dates in the range.
3. Report the rule's percentile among them on the objective metric.

Both the alternatives and the rule are measured here on a frictionless curve -- fully invested
while holding, flat otherwise, no costs -- so the comparison is about timing alone and not about
how the costs fell. The rule's own reported figures are after costs. JOB.md section 6 has the
generator and the placement exactly.

A drawdown rule that beats buy-and-hold only by being out of the market during 2008 should
also beat most random placements. If it doesn't, the "timing" is luck.

### Multiple-testing correction

A sweep of N parameter sets reports the best one, and the best of N is biased upwards (see
[`docs/FALSE_DISCOVERY.md`](../../docs/FALSE_DISCOVERY.md)). Sweep points are highly correlated, so N overstates the
number of independent tries. The engine estimates an **effective N**: the number of principal
components needed to explain 95% of the variance in the sweep points' daily return series.
The corrected p-value is `1 - (1 - p)^N_eff`, where `p` is the random-timing p-value of the
selected point.

This is a first approximation, and it should be calibrated the way FALSE_DISCOVERY calibrates
its threshold: run the full sweep on null paths and check that the corrected p-value rejects
at the rate it claims. Until that calibration exists, results carry an
`UNCALIBRATED_CORRECTION` warning.

### Plateau ratio

For the selected point, take every sweep point one step away in any single swept dimension.
The edge of a point is its objective metric minus the primary benchmark's over the same
window, signed so that positive is better. The plateau ratio is the median neighbour edge
divided by the selected edge. Near 1 is a broad plateau; near 0 or negative means a spike.

### Outcome range (bootstrap)

A stationary block bootstrap (Politis and Romano) of the underlying's daily returns, with a
mean block length of 63 trading days so crashes and recoveries stay intact, 1,000 paths.
Synthetic history is resampled along with real history. The selected rule is re-run on each
path, and so is buy-and-hold of that same path: on a path the benchmark is always the instrument
itself, whatever the form's primary benchmark is, because the question a path answers is whether
the timing beat holding what was timed. Reported: CAGR percentiles for both, the probability the
rule beats holding, and the share of paths with a drawdown worse than -90%.

The rule is re-run on each path rather than resampling its own returns, because a drawdown
rule's trades depend on the path.

---

## Windows

Every metric is reported for three windows:

- `in_sample`: start to `oos_split`, where the sweep is ranked,
- `out_of_sample`: `oos_split` to end, where the selected parameters are judged,
- `full`: the whole range.

If `oos_split` is null, only `full` exists, and `oos_benchmark` is judged on `full` with an
`NO_OUT_OF_SAMPLE` warning.

---

## Metric definitions

All computed from daily closing equity, after costs. Cash earns nothing: the only cash yield the
job can honour today is `none` (JOB.md section 1), so Sharpe and Sortino below are on raw daily
returns, and become excess returns when a rate series exists. JOB.md section 4 has every
definition exactly, including the order of operations.

| metric | definition |
|---|---|
| `total_return_pct` | `100 * (E_end / E_start - 1)` |
| `cagr_pct` | `100 * ((E_end / E_start)^(365.25 / calendar_days) - 1)` |
| `volatility_pct` | Standard deviation of daily returns × √252, in percent. |
| `sharpe` | Mean daily return divided by the sample standard deviation of daily returns, × √252. |
| `sortino` | Mean daily return divided by the downside deviation of daily returns (below zero), × √252. |
| `max_drawdown_pct` | Largest peak-to-trough fall in equity, as a negative percent. |
| `max_drawdown_days` | Longest stretch, in calendar days, from a peak to the recovery of that peak (or the end of the window if not recovered). |
| `calmar` | `cagr_pct / abs(max_drawdown_pct)` |
| `ulcer_index` | Square root of the mean squared percent drawdown from the running peak. |
| `time_in_market_pct` | Percent of sessions with a non-zero position. |
| `n_entries` | Entries that started in the window. |
| `win_rate_pct`, `avg_trade_return_pct`, `worst_trade_pct` | Over trades closed in the window. Null when there are none. A position still open at the end is marked to market and flagged `open`. |

A trade that straddles `oos_split` is counted twice, for two different questions, and JOB.md
section 4 fixes both: its *entry* belongs to the window it started in (`n_entries`, and the
`window` field in `stats.trades`), and its *result* to the window it closed in (`win_rate_pct`,
`avg_trade_return_pct`, `worst_trade_pct`).

---

## Warnings

Short codes the UI shows under the verdict, each with a sentence of explanation.

| code | when |
|---|---|
| `MOSTLY_SYNTHETIC` | Over half the range uses synthetic history. |
| `EXERCISE_ONLY_DATA` | Any series is `exercise_only`. |
| `NO_OUT_OF_SAMPLE` | `oos_split` was null. |
| `NO_SEARCH_CORRECTION` | Nothing was swept, so no multiple-testing correction was applied. |
| `UNCALIBRATED_CORRECTION` | The effective-N correction has not been calibrated on null data. |
| `FEW_TRADES_IN_SWEEP` | Some sweep points were excluded from ranking for having fewer than `objective.min_trades` entries. |
| `OPEN_POSITION_AT_END` | The selected rule was still holding at the end of the range. |
| `SWEEP_EDGE` | The selected value is at the edge of the swept range, so the optimum may lie outside it. |

---

## What the LLM receives

The full results JSON, together with the form and its provenance. Its analysis:

- may not contradict or soften the grade, and never describes a `fragile` or
  `insufficient_evidence` result as a finding,
- refers to the stats rather than repeating them, and adds only what the numbers don't say on
  their own: why a gate failed, what the trades have in common, which assumption is doing the
  work,
- uses only numbers present in the results.
