# Pathologies

Three biases injected on purpose, and what each one was worth on a panel whose
true signal strength is known. Regenerate with `python -m research.pathology`;
the assertions live in `research/tests/test_pathologies.py`.

**Every number here is a statement about this pipeline on synthetic data.** The
magnitudes depend on the departure rate, the hazard tilt and the panel length,
all of which were chosen to make an effect measurable rather than to match any
real index. What transfers is the direction, the mechanism, and the order of
magnitude -- not the decimals.

| | honest | with the bias | ratio |
|---|---|---|---|
| survivorship (per period, per unit exposure) | +0.000289 | +0.000388 | 1.34x |
| lookahead (out-of-sample R-squared) | +0.000214 | +0.658763 | 3081.69x |
| delisting (mean forward return in the study) | -0.004702 | -0.001512 | 0.32x |
| assumed exit (total return over the panel) | +0.328436 | +0.546414 | 1.66x |

---

## 1. Survivorship

Panel: 390 securities over 1250 periods, planted lambda +0.000500, departure hazard 0.0008 per period tilted towards *high* exposure,
and delisting returns drawn from the bimodal model in `panel.py`. 190 names left. A survivors-only universe keeps 200 of them.

### The index level

| | |
|---|---|
| equal-weight return, point-in-time | -1.86% a year |
| equal-weight return, survivors only | +2.73% a year |
| **difference** | **+4.59% a year** |

This is the part of survivorship bias whose sign is not negotiable. The survivors
are the names that did not fall over; an index built from them returns more than
the one anybody could have held, every time.

### The premium

| | |
|---|---|
| lambda, point-in-time | +0.000289 |
| lambda, survivors only | +0.000388 |
| inflation | **1.34x** |
| standard error of either estimate | 0.000041 |
| information coefficient | +0.0263 -> +0.0231 |

The mechanism: the hazard here is tilted towards names with a *high* exposure --
the ones a long-short book following this signal is long. Their exits are losses
the book took. Remove the names and the losses go with them, and what is left is
a signal whose failures have been deleted from the record.

### The sign is not a property of survivorship

The identical experiment with the hazard tilted the other way, so that the
failures are among the names the book is *short*:

| | |
|---|---|
| lambda, point-in-time | +0.000629 |
| lambda, survivors only | +0.000449 |
| ratio | **0.71x** |

The bias *deflates* the premium. Being short a name that fell was a gain, and a
survivors-only universe removes it.

This milestone's first run was configured that way and reported survivorship bias
shrinking the premium -- the opposite of what everybody expects to read. It was
not a bug, and tuning until the expected sign appeared would have been the actual
mistake. **Survivorship bias moves a measured premium in whichever direction the
failures were correlated with the signal.** The size is the reliable part, and in
both directions here it is tens of percent of the premium being measured.

**The guard.** The engine gives a strategy no way to see a non-member
(`docs/IDENTITY.md`), and `Panel.frame` holds no row for a security on a date it
was not a member. Survivorship has to be *added* by a deliberate step --
`research.pathology.survivors_only` -- which is what this measurement does.

## 2. Lookahead

### The gross case: tomorrow's return as today's feature

| | |
|---|---|
| out-of-sample R-squared, honest | +0.00021 |
| out-of-sample R-squared, leaking | **+0.65876** |
| information coefficient | +0.0306 -> +1.0000 |
| its t-statistic | +12.2 -> ~1e6 |

The leaked information coefficient is **1.0 to four decimal places**, on every
single date -- a perfect ranking of the cross-section, over and over. Its only
variation is at the fifth decimal, where the model blends the leak with the
honest feature, so the t-statistic that falls out is six digits long. That number
is arithmetically correct and tells you nothing except that the question was
wrong: a statistic whose sampling variation is rounding error is not evidence
about a population, and a t-statistic of a hundred thousand has never once in the
history of finance meant what it appears to mean.

**This is the smell.** An out-of-sample R-squared above 2% on
daily cross-sectional returns is not a discovery, it is a leak. No honest daily
feature reaches it: a number that size implies an information coefficient no
published factor sustains. If a result ever looks like this, the first hypothesis
is a leak. The second hypothesis is also a leak.

### The realistic case: a feature window that overlaps the label

A ten-period forward label, and a feature that has seen the fifth of those ten
periods. Nobody writes this on purpose; it is what an off-by-a-window arithmetic
error produces.

| | |
|---|---|
| out-of-sample R-squared, honest | +0.00034 |
| out-of-sample R-squared, overlapping | +0.06725 |
| information coefficient | +0.0733 -> +0.3108 |

Smaller, and much easier to believe. This is the one that gets published, and it
is why `splits.py` purges on **label windows** rather than on dates: the overlap
that matters is between what a label is made of and what a feature has already
seen, and neither of those is visible from a date alone.

**The guard.** Every transform in `research/features.py` is causal, and
`test_features.py` proves it structurally rather than transform by transform: it
perturbs the final date and asserts no feature value on any earlier date moves. A
new transform is covered by that test the moment it is registered. The leak above
had to be requested by name.

## 3. Delisting

188 names taken to **-100%** and removed from the universe. A
ten-period forward label has no ten-period window for a name with four periods
left, so a pipeline has to decide what that label is. The two policies in
`labels.py`, on the same panel:

| | |
|---|---|
| labels lost by dropping truncated windows | 1658 |
| cumulative return carried by those labels | -475.6 |
| mean forward return in the study, compounded | -0.004702 |
| mean forward return in the study, dropped | **-0.001512** |
| labels that swallowed a departure whole, compounded | 90 |
| labels that swallowed a departure whole, dropped | **9** |
| worst single label, either policy | -1.0000 |
| positions force-exited by the book | 156 |
| return earned in departure periods | +0.03434 |

The average forward return in the study goes from -0.47% to -0.15% -- the naive policy
removes 68% of the average
loss. Those rows are not adjusted for, or modelled around, or argued about: they
are *absent*. Nobody decided to exclude them. The label window had nowhere to end
and the row went away.

**And the worst case survives, which is what makes it dangerous.** Of the ten
labels whose window contains a given departure, exactly one is untruncated -- the
window that ends precisely on the departure date -- and it survives both policies.
So 90 ruinous labels become 9, but the minimum of the sample is unchanged at -1.00.

A study that sanity-checks its own worst observation therefore finds it present,
correct, and catastrophic, and concludes the delistings are being counted. They
are being counted once each instead of ten times each.

(That paragraph is here because the first draft of this report asserted the
opposite -- that the surviving rows were the ones further back -- and the
assertion failed against the measurement. The counts above are now counted rather
than argued.)

The estimated lambda moves too, from +0.000366 to +0.000362 against a standard error of 0.000056.
That one is reported and not leaned on: the hazard in this panel is untilted, so
what a -100% label does to a cross-sectional *slope* depends on the exposure the
dying name happened to have, and across one panel that is mostly the seed. The
level effect above is the systematic one.

**The portfolio side holds independently.** A book long a name that goes to -100%
earns -100% on that position, because the departure return is the final period of
that name's return series and the position is still on when it lands. The loss is
in `PortfolioResult.active_returns`, and `delisting_loss` reports it separately so
it cannot be mistaken for anything else.

**What is not measured here.** The engine's half -- force-liquidation at the last
price, logged distinctly in the blotter, proceeds credited to cash -- is asserted
by `tests/test_universe.cpp` ("a held security that leaves the universe is
liquidated") and is joined to this pipeline at M9, where the same strategy runs
both ways and the two have to agree. Nothing in this file has run the C++ ledger.

And the engine's forced exit is **optimistic**: it liquidates at the last
available price, where a real delisting return is frequently far worse. That is a
known limitation with its own entry in `docs/STATUS.md`, and nothing in this
milestone fixes it. What this milestone establishes is what it would be worth if
it were wrong.

## 4. The assumed exit

What ignoring the delisting return is worth, measured by running one strategy
over one panel under both settings of `DelistingReturnModel`.

`last_price` liquidates at the final quoted price and applies no delisting
return. It is the default, it is what the engine does, and it is what a backtest
does when nobody has asked the question. `apply_true_return` uses what the
generator knows actually happened -- a bimodal draw by cause, distress with mass
near total loss against voluntary exits that pay a small premium.

211 departures, 64% distressed, worst -100%; Sharpe +1.91 -> +4.30

| | |
|---|---|
| total return, assuming a zero delisting return | **+54.64%** |
| total return, applying the real one | +32.84% |
| **flattery** | **+21.80%** |
| Sharpe, assuming | **+4.30** |
| Sharpe, applying | +1.91 |
| **flattery** | **+2.39 of Sharpe** |

The assumption **more than doubles the Sharpe**. Not a rounding difference, not a
second-order correction: a book that is long the names which fail, credited with
selling them at the last print, looks like a different strategy from the one that
actually ran.

### And the sign follows which leg failed, again

Run the identical experiment with the hazard tilted the other way, so the
failures are among the names the book is *short*:

| | |
|---|---|
| difference in total return | -40.95% |
| difference in Sharpe | -0.03 |

Ignoring the delisting return now *understates* the return substantially while
barely moving the Sharpe -- the gains arrive in a few large lumps, which raises
the mean and the volatility together. Same rule as survivorship: the direction is
a property of the correlation between failure and the signal, and only the
magnitude is reliably large.

### Why the default stays the flattering one

Because changing it silently would move every number measured under it. The
switch is named on both sides -- `research/delisting.py` and the engine's
`include/marketscan/exec/delisting.hpp`, spelled identically -- and
`last_price` remains the default so that turning it on is a decision somebody
made. When CRSP delisting returns arrive, `apply_true_return` is the whole of
the change.

The engine reaches the same conclusion independently, on its own fixture and
through its own ledger: `tests/test_parity.cpp` runs the decile rule over a
panel with departures under both settings and measures 30.10 points of return
and 3.55 of Sharpe. Nothing in *this* file has run that path -- the two
measurements are separate by construction, which is what makes them a
cross-check rather than a restatement.

See **the assumed exit** in [`EXECUTION.md`](EXECUTION.md) for the shared name
and what each layer substitutes.

---

## What this milestone is for

Not to prove the guards exist -- the tests do that. To put a number on what each
bias is worth, so that a future result which happens to look good can be checked
against the size of the mistakes that produce results which look good.

