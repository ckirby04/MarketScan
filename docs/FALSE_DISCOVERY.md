# False discovery

What this pipeline finds in data that contains nothing at all.

Regenerate with `python -m research.sweep`. The threshold is importable as
`research.thresholds.NULL_SHARPE_THRESHOLD`, and `research/tests/test_sweep.py`
asserts that the constant and this measurement still agree -- so a future result
is judged against a measured bar rather than a remembered one.

> **The bar, for a search of this size on data of this length:**
> an annualized Sharpe of **1.93**.

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
| mean Sharpe across all runs | +0.027 |
| standard deviation | 0.546 |
| range | -1.65 to +1.76 |
| skew / excess kurtosis | -0.23 / +0.00 |

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
| information coefficient | **17.7%** | 6.2% | 1512 |
| portfolio Sharpe | 3.7% | 0.3% | 1512 |
| estimated lambda | 5.6% | 0.0% | 108 |

**The Sharpe is calibrated. The information coefficient is not.**

At 3.7%, the Sharpe's t-statistic rejects at about the
rate it promises. The information coefficient rejects at 17.7% -- more than three times nominal -- and the reason is
visible in the breakdown by horizon:

| horizon | IC | Sharpe |
|---|---|---|
| 1 | 12.1% | 2.2% |
| 5 | 16.1% | 4.0% |
| 20 | 24.8% | 5.0% |

The IC's false-positive rate **rises with the horizon**, from
12.1% at one period to
24.8% at twenty. Consecutive information
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

The six panels give six realizations of best-of-252: +1.66, +1.28, +1.33, +1.22, +1.76, +1.49 -- mean
**+1.46**, standard deviation 0.22.

| variants tried | mean best | median | 90th | 95th | 99th |
|---|---|---|---|---|---|
| 1 | +0.02 | +0.05 | +0.66 | +0.87 | +1.15 |
| 10 | +0.80 | +0.81 | +1.19 | +1.30 | +1.66 |
| 50 | +1.16 | +1.12 | +1.49 | +1.66 | +1.76 |
| 100 | +1.29 | +1.22 | +1.66 | +1.76 | +1.76 |
| 252 | +1.46 | +1.49 | +1.76 | +1.76 | +1.76 |

That table is produced by resampling variants within a panel, and **its upper
percentiles at large N are a floor, not an estimate.** Drawing 252 variants from a
panel that has 252 returns the panel maximum every time, so the 90th, 95th and
99th percentiles all collapse onto +1.76 -- the
largest Sharpe anywhere in the sweep. Six panels cannot resolve the tail of a
best-of-252 distribution, and pretending otherwise would be the same error this
whole document is about.

### So the threshold is built from what the sweep does estimate

1. The per-variant null Sharpe distribution, from all 1512 runs:
   mean +0.027, standard deviation 0.546, close to normal.
2. The maximum of *n* draws from a normal is Gumbel, with a location and scale
   that follow from the tail.
3. The observed mean best-of-252 of +1.46 calibrates
   *n*: **252 correlated variants behave like
   144 independent ones.** Variants sharing a feature or
   a horizon are not separate tries, and assuming they were would set the bar too
   high -- a different way of being wrong, not a safer one.
4. The threshold is the 95th percentile of that calibrated distribution.

| | |
|---|---|
| expected best-of-252, independent | +1.56 |
| observed mean best-of-252 | +1.46 |
| effective independent tries | 144 |
| 95th percentile, calibrated | **+1.93** |
| 95th percentile, assuming independence | +2.00 |

The observed mean best sits *below* what independence predicts, which is the
expected direction and a check that step 3 is doing something real: correlated
tries explore less.

### The single best variant of the whole sweep

| | |
|---|---|
| configuration | `ret.compound20|h1|gbm,max_depth=3` |
| Sharpe | **+1.76** |
| information coefficient | -0.0016 (t = -0.44) |
| out-of-sample R-squared | +0.00040 |

A specific, reasonable-looking configuration, on a panel with no signal in it at
all.

Note what its own statistics say: an information coefficient of -0.0016
at t = -0.44, and an out-of-sample R-squared of +0.00040 --
**negative**, meaning its forecasts were worse than predicting zero. The Sharpe is
the only flattering number it has, and the Sharpe is the number that would have
been reported. Nothing about the way it was found differs from the way a real one
would be found.

## The threshold

> **A Sharpe of 1.93.** A search of 252 variants over three years
> of this data produces a best result at or above it one time in twenty **when
> nothing is there at all**.

### This is a problem, and it should be said plainly

A bar of 1.93 is higher than most real strategies ever reach, and it
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

## The information coefficient, calibrated

The table above says the IC's t-statistic is not calibrated. This says what to
use instead.

The same null, the same generator, 1512 runs over
6 panels across 6 horizons, with the portfolio stage
switched off because an information coefficient does not need a book. For each
horizon: the |IC| a t-statistic of 1.96 implicitly demands, how often that rule
fires when nothing is there, and the |IC| the null actually produces.

| horizon | t = 1.96 demands | that rule's false positives | measured single-test bar | measured best-of-42 bar |
|---|---|---|---|---|
| 1 | 0.0075 | **11.9%** | 0.0088 | 0.0152 |
| 2 | 0.0085 | **14.7%** | 0.0107 | 0.0196 |
| 5 | 0.0118 | **15.5%** | 0.0163 | 0.0283 |
| 10 | 0.0152 | **21.8%** | 0.0243 | 0.0423 |
| 20 | 0.0183 | **26.6%** | 0.0324 | 0.0535 |
| 40 | 0.0225 | **21.4%** | 0.0335 | 0.0590 |

Read the second and fourth columns against each other. They are the same
quantity -- an absolute information coefficient -- so the gap between them is the
size of the error, in the units the statistic is reported in, with no arithmetic
required.

The t-rule is worst at horizon 20, where it fires
26.6% of the time on data with nothing in it, and best at
horizon 1 at 11.9%. It is
anti-conservative at every horizon measured, which is the direction that matters:
a test that under-rejects wastes discoveries, and a test that over-rejects
manufactures them.

**Absolute values throughout.** A researcher who finds a feature with an IC of
-0.05 negates the feature, so the statistic a search maximizes is |IC| and every
bar here is on |IC|. The sign flip doubles the tries, and the effective-tries
calibration absorbs it without being told.

The t-statistic is still computed and still reported -- it is a serviceable
screen, and its measured false-positive rate travels with it at the call site
rather than living only here. What it is not is evidence.

## What moves the bar

A bar of 1.96 invites the question of what to do about it, and there are exactly
two levers: test fewer things, or have more data. They are not equally strong,
and the difference is not a matter of opinion.

The bar is the spread of null Sharpes multiplied by an extreme-value factor. The
spread falls as **1/sqrt(sample length)**. The factor grows as roughly
**sqrt(2 ln n)** in the number of independent tries. Square root against
logarithm: the sample wins, and it is not close.

Measured, not asserted -- 300 further null runs at
3 sample lengths, 25 variants over
4 panels each:

| sample | null Sharpe spread | predicted 1/sqrt(years) | bar, 25 variants | bar, 252 variants |
|---|---|---|---|---|
| 3 years | 0.575 | 0.577 | 1.78 | 2.03 |
| 8 years | 0.381 | 0.354 | 1.15 | 1.34 |
| 20 years | 0.217 | 0.224 | 0.68 | 0.77 |

The second and third columns are a check on the whole construction: the measured
spread of null Sharpes tracks 1/sqrt(years) across the range, which is what
licenses reading the table as arithmetic rather than as three disconnected
experiments.

The last column uses the effective try count M8 measured for the full family
(144 independent tries out of
252 variants), applied at each sample length.
That assumes the effective count is a property of the family rather than of the
sample. The assumption was checked and it **drifts by 53%** across these lengths, which is not
nothing: the effective count is estimated from the mean best over four panels, so
it is the noisiest quantity on the page. Read that column as indicative and the
measured one beside it as the result. Both tell the same story, which is the
reason the drift does not change the conclusion.

One correction worth recording. The arithmetic this section was asked to verify
sketched the small-search bar at roughly 1.2, on the assumption that 25 variants
behave like 8 independent tries. Measured, they behave like
21, and the bar is
1.78. **The variant lever is weaker
than sketched** -- cutting the search by a factor of ten buys less than expected,
which makes the conclusion below stronger rather than weaker.

### The conclusion, without hedging

**Sample length dominates variant discipline.**

Twenty years with the full 252-variant sweep
gives a bar of 0.77. Three years with a tenth of the
variants gives 1.78. The long sample with *no* discipline
beats the short sample with a lot of it, and it is not a close call:
lengthening the sample cuts the bar by
62%, while cutting the search
by a factor of ten cuts it by 12%.

An equity factor with a real Sharpe of 0.3 to 0.8 -- which is what the honest end
of the literature looks like -- is invisible at 1.78 and
detectable at 0.77. On three years of data this pipeline
cannot see a real factor at all, however carefully the search is conducted.

That makes the data source a **research-design constraint rather than a
convenience**. CRSP back to 1926 is worth more than any modelling improvement
available to this project right now, and it is not a close call either. Nothing
in `research/` will produce a defensible result on three years of anything.

## What this number is not

- **It is not transferable to a different search.** The threshold is a function of
  how many variants were tried (252, behaving like
  144), the length of the sample (three years), and how
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

