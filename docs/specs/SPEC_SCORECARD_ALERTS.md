# Scorecard, scans and alerts

Draft for review, 2026-10-22. Nothing here is built yet.

A finished run gets a **scorecard**: six sub-grades and an overall grade. Saved strategies
can be **re-run on a schedule**, and any run, whether started by hand or by a scan, whose
scorecard clears a threshold sends an **email**.

## 1. Where the scorecard is computed

In the engine, as a new `scorecard` block in the Results, and independently in
`research/grading.py`. The two are held to exact agreement by `test_grading.py`, as every
other number in the Results is. The web page only draws it (SPEC_RESEARCH_VIEW §2: the view
computes nothing). The scorecard is a pure function of fields already in the Results, so it
adds no new measurement, only a reading of existing ones.

**Window.** Every sub-grade reads the `out_of_sample` window when the run has one, and `full`
otherwise. The scorecard names the window it used. Grading the years not used to choose a
setting is the main defence against a sweep's best point grading itself.

## 2. Sub-grades

Each is a score from 0 to 100, drawn as a letter: **A** 85+, **B** 70+, **C** 55+, **D** 40+,
**F** below 40. Within a metric, the score is linear between the anchors below and clamped at
both ends, so 12% a year scores 76 (70 + 2/5 of the 15 points to the next anchor), a low B.

| sub-grade | reads | 100 | 85 (A) | 70 (B) | 55 (C) | 40 (D) | 0 |
|---|---|---|---|---|---|---|---|
| **Return** | `cagr_pct` | 25 | 15 | 10 | 6 | 2 | -10 |
| **Risk** | `max_drawdown_pct` | -5 | -15 | -25 | -35 | -50 | -90 |
| **Volatility** | `volatility_pct` | 5 | 10 | 15 | 20 | 30 | 60 |
| **Risk-adjusted** | `sharpe` | 2.0 | 1.0 | 0.75 | 0.5 | 0.25 | -0.5 |
| **Versus holding** | `cagr_pct` minus the first benchmark's, in points, and the benchmark's drawdown minus the rule's | see below | | | | | |
| **Robustness** | the existing verdict | supported 95, fragile 60, not supported 45, insufficient evidence 30 | | | | | |

**Versus holding** is the mean of two scores: return difference (anchors +6, +3, +1, -1, -3,
-10 points) and drawdown difference (anchors +30, +15, +5, 0, -5, -30 points, positive meaning
the rule fell less). It is `not_applicable` when the run has no benchmark, and then it drops
out of the overall grade instead of counting as zero.

**Robustness** is the engine's existing verdict (JOB.md §10) as a sub-grade. The verdict stays
exactly as it is and is still shown on its own.

Undefined metrics (for example a Sharpe ratio with zero volatility) score 0, and the
scorecard says why.

## 3. The overall grade

The weighted mean of the applicable sub-grades, re-normalised when one is `not_applicable`:

| Return | Risk | Volatility | Risk-adjusted | Versus holding | Robustness |
|---|---|---|---|---|---|
| 25 | 20 | 10 | 15 | 15 | 15 |

The overall grade is independent of the verdict, as asked. **The consequence to accept:**
a lucky sweep point with strong numbers and failing checks can still score around B. Robustness
counts for 15 of 100. Section 5's alert rule is where that is guarded against.

**Data quality.** On `exercise_only` data the scorecard carries the same "Unverified" mark as the
verdict. It does not change the score.

## 4. Scheduled scans

A **watchlist** of saved strategies: any finished run's form can be added from the results
("Watch this"). A scan re-runs every watched form, including its sweep if it has one, and grades
each result like any other run.

**Scans only find something new when the data is new.** The same form on the same data gives
byte-identical Results (SPEC_RESEARCH_VIEW §8), so a scan is worth running only after the prices
change. Today the prices are fetched by hand on this machine and uploaded. Scans therefore need:

1. **A data refresh job.** A Cloud Run job that runs the existing fetchers (Sharadar and FRED),
   rebuilds the recipes and uploads to the bucket. It needs `SHARADAR_API_KEY` and
   `FRED_API_KEY` as Secret Manager secrets, which you create; no key is ever read or printed by
   a session. Sharadar's licence is for personal use and that does not change: the data stays in
   the private bucket.
2. **Cloud Scheduler.** Data refresh after the US close on weekdays, then the scan.
3. **Cost limits.** At most N watched forms, and a cap on the variants each sweep may produce,
   so a scan cannot run away. Proposed: 20 forms, 200 variants each.

**Not proposed:** scans that invent new strategies by themselves (the assistant generating
variations overnight). That would search far more variations than the multiple-testing
correction can see, and the best of them would grade well by chance. Watched forms, re-run on
new data, are the honest version of "keep looking".

## 5. Alerts

**Rule.** Email when a run's overall score is at or above a threshold you set (proposed: 80).
Optionally also require a minimum Robustness. **Recommended: on by default, at "supported".**
Without it, scans will email you the lucky sweep points from §3.

**Once per crossing.** A watched strategy alerts when it first clears the threshold, and again
only after it has dropped below and come back. Runs you start by hand alert once per run.

**Email.** Sent from the service by an email API: Resend (free up to 3,000 a month) or SendGrid.
Plain SMTP through Gmail with an app password also works. You create the account and store
its key as a Secret Manager secret, and the address to email is set as a service setting.

**Wording.** It follows SPEC_RESEARCH_VIEW §2: no "buy", "recommended", "expected return" or
"good idea". Subject: `MarketScan: SPY drawdown rule scored 84 (B), historical`. The body has
the scorecard, the verdict, the window, and a link to the run.

## 6. On the page

The scorecard goes right after the verdict block and before Why: the overall grade large, the
six sub-grades as a row of letters, each opening to the figure it read and its anchors. The
order verdict → why → details → analysis is otherwise unchanged.

## 7. Build order

1. JOB.md §11 (this rubric, made exact), then the engine and Python, cross-tested, with the
   scorecard in the results schema.
2. The scorecard on the page.
3. Alerts on runs you start (service: rule, email, deduplication).
4. Watchlist and scans, then the data refresh job and the scheduler.

Steps 1–3 are useful alone. Step 4 is the largest and depends on the data refresh.

## 8. Open questions

- The anchors in §2 are a first proposal. They favour unleveraged equity-like strategies; a
  leveraged fund will rarely score well on Risk or Volatility, which is arguably correct.
- Should sub-grades show `+`/`-` (B+)? The table above has five bands; pluses add nothing the
  0–100 score does not already say.
