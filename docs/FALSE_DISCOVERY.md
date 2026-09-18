# False discovery

What this pipeline finds in data that contains nothing at all.

Regenerate with `python -m research.sweep`. The threshold is importable as
`research.thresholds.NULL_SHARPE_THRESHOLD`, and `research/tests/test_sweep.py`
asserts that the constant and this measurement still agree -- so a future result
is judged against a measured bar rather than a remembered one.

> **The bar, for a search of this size on data of this length:**
> an annualized Sharpe of **1.96**.

## The experiment

**6 independently seeded synthetic panels**, each
100 names over 750 business days, with
**lambda = 0**: returns are a market factor plus noise, and the exposure column is
an AR(1) process wired to nothing. Names still enter and leave, so the portfolio
layer still has forced exits to handle -- but the departure hazard is *untilted*,
because a hazard that depends on the exposure makes the exposure predict returns
through the departure channel and puts a real signal into a panel that has to be
empty. The first version of this sweep had exactly that, and it was re-run. A test
pins it now.

Against each panel, **252 pipeline variants** -- 21 feature sets, three
horizons, four model configurations -- for **1512 runs** in
total. Each produces an information coefficient, an estimated lambda, an
out-of-sample R-squared, and a frictionless Sharpe from a constrained long-short
book.

Nothing in any of it is real. Every number below is the pipeline measuring itself.

## What a single variant finds

| | |
|---|---|
| mean Sharpe across all runs | +0.011 |
| standard deviation | 0.579 |
| range | -2.10 to +1.81 |
| skew / excess kurtosis | -0.26 / +0.06 |

The mean is zero to three decimal places, which is the first thing to check: a
null panel that produced a positive average Sharpe would be a null panel with
something in it. The distribution is close enough to normal to build on, and the
threshold below does.

### False discovery rate, one variant at a time

The fraction of runs that cleared a conventional significance bar. On data with no
signal a calibrated two-sided test rejects 5% of the time at |t| > 1.96,
so this measures whether the machinery delivers the rate it advertises.

| statistic | \|t\| > 1.96 | \|t\| > 2.58 | trials |
|---|---|---|---|
| information coefficient | **16.6%** | 5.0% | 1512 |
| portfolio Sharpe | 4.4% | 0.8% | 1512 |
| estimated lambda | 5.6% | 0.0% | 108 |

**The Sharpe is calibrated. The information coefficient is not.**

At 4.4%, the Sharpe's t-statistic rejects at about the
rate it promises. The information coefficient rejects at 16.6% -- more than three times nominal -- and the reason is
visible in the breakdown by horizon:

| horizon | IC | Sharpe |
|---|---|---|
| 1 | 8.9% | 4.0% |
| 5 | 17.3% | 3.6% |
| 20 | 23.6% | 5.8% |

The IC's false-positive rate **rises with the horizon**, from
8.9% at one period to
23.6% at twenty. Consecutive information
coefficients at a horizon of h are computed from returns that share h-1 periods,
and `PipelineResult.ic_t` corrects for that with a Newey-West standard error at
h-1 lags. The correction is in the right direction and is **not enough**: at
twenty lags the Bartlett estimator is itself noisy, and it under-corrects.

The practical consequence, stated as a rule: **do not use the IC t-statistic in
this codebase as an inferential statistic.** It is a fine descriptive measure of
how well a score ranks a cross-section. As a significance test at a long horizon
it is wrong by a factor of four, and it is wrong in the flattering direction.

The lambda row is a different kind of number and its trials column says so. That
estimate is measured against the panel's raw exposure, so it does not depend on a
variant's features or model at all -- only on the panel, the horizon, and how far
the feature windows warmed up. 1512 runs carry
108 distinct estimates, and those are themselves
highly correlated: six warm-up lengths per (panel, horizon) differing only in
where the sample starts. Read the denominator as an upper bound on the trials.

On it, the Fama-MacBeth estimator rejects at 5.6%. That
is the estimator behaving -- and it is the one M6 measures lambda with, which is
the reassuring half of this table.

## What a *search* finds

Nobody reports the average of the variants they tried. They report the best one.
So the distribution that matters is of the maximum.

The six panels give six realizations of best-of-252: +1.54, +1.33, +1.29, +1.81, +1.26, +1.35 -- mean
**+1.43**, standard deviation 0.21.

| variants tried | mean best | median | 90th | 95th | 99th |
|---|---|---|---|---|---|
| 1 | +0.00 | +0.07 | +0.72 | +0.91 | +1.21 |
| 10 | +0.84 | +0.83 | +1.21 | +1.35 | +1.81 |
| 50 | +1.20 | +1.18 | +1.54 | +1.81 | +1.81 |
| 100 | +1.32 | +1.28 | +1.81 | +1.81 | +1.81 |
| 252 | +1.43 | +1.35 | +1.81 | +1.81 | +1.81 |

That table is produced by resampling variants within a panel, and **its upper
percentiles at large N are a floor, not an estimate.** Drawing 252 variants from a
panel that has 252 returns the panel maximum every time, so the 90th, 95th and
99th percentiles all collapse onto +1.81 -- the
largest Sharpe anywhere in the sweep. Six panels cannot resolve the tail of a
best-of-252 distribution, and pretending otherwise would be the same error this
whole document is about.

### So the threshold is built from what the sweep does estimate

1. The per-variant null Sharpe distribution, from all 1512 runs:
   mean +0.011, standard deviation 0.579, close to normal.
2. The maximum of *n* draws from a normal is Gumbel, with a location and scale
   that follow from the tail.
3. The observed mean best-of-252 of +1.43 calibrates
   *n*: **252 correlated variants behave like
   81 independent ones.** Variants sharing a feature or
   a horizon are not separate tries, and assuming they were would set the bar too
   high -- a different way of being wrong, not a safer one.
4. The threshold is the 95th percentile of that calibrated distribution.

| | |
|---|---|
| expected best-of-252, independent | +1.65 |
| observed mean best-of-252 | +1.43 |
| effective independent tries | 81 |
| 95th percentile, calibrated | **+1.96** |
| 95th percentile, assuming independence | +2.12 |

The observed mean best sits *below* what independence predicts, which is the
expected direction and a check that step 3 is doing something real: correlated
tries explore less.

### The single best variant of the whole sweep

| | |
|---|---|
| configuration | `ret.rank60|h1|ridge,alpha=1` |
| Sharpe | **+1.81** |
| information coefficient | +0.0067 (t = +1.62) |
| out-of-sample R-squared | -0.00035 |

A specific, reasonable-looking configuration, on a panel with no signal in it at
all.

Note what its own statistics say: an information coefficient of +0.0067
at t = +1.62, and an out-of-sample R-squared of -0.00035 --
**negative**, meaning its forecasts were worse than predicting zero. The Sharpe is
the only flattering number it has, and the Sharpe is the number that would have
been reported. Nothing about the way it was found differs from the way a real one
would be found.

## The threshold

> **A Sharpe of 1.96.** A search of 252 variants over three years
> of this data produces a best result at or above it one time in twenty **when
> nothing is there at all**.

### This is a problem, and it should be said plainly

A bar of 1.96 is higher than most real strategies ever reach, and it
was produced by three years of data and a fortnight's worth of variants. A
search of that size over a sample that short **cannot distinguish a genuine
signal from the best of its own noise** unless the signal is extraordinary.

That is not an argument for a better model. It is an argument about the
experiment: fewer variants, longer samples, and the threshold computed and
written down *before* the result is admired. The bar falls fast as the search
narrows -- the best-of-N table above is the map -- and a real signal then has
somewhere to show up.

Anything this project reports from a search of this shape, on data of this
length, below this Sharpe, is not evidence. Including anything it reports
later, on real data, having forgotten this page.

## What this number is not

- **It is not transferable to a different search.** The threshold is a function of
  how many variants were tried (252, behaving like
  81), the length of the sample (three years), and how
  correlated the variants are. More variants raise it; a longer sample lowers it.
- **It is frictionless.** The research layer charges no commission and crosses no
  spread, by design -- the C++ engine owns execution. Costs would lower the Sharpes
  being judged as well as the bar, and not by the same amount.
- **It is synthetic and Gaussian.** The null is a market factor plus normal noise.
  Real returns have fat tails, volatility clustering and cross-sectional structure
  this panel does not contain, all of which give a search more to find. Treat the
  threshold as a floor.
- **It rests on 6 panels.** The per-variant distribution is
  well estimated from 1512 runs; the calibration of effective tries rests on six
  observations of the maximum. More panels would tighten it, and that is the
  single highest-value thing to do to this measurement.

## Why this is the milestone that matters

Everything before it establishes that the instrument responds to a signal. This
establishes what it does when there is none -- and that is the number a result has
to be compared against before it means anything. A pipeline that has not measured
its own false discovery rate is not a measuring instrument. It is a search, and a
search always finds something.

