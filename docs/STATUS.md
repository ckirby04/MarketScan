# Status

A handoff, not a changelog. What is proven and by which measurement, what is
approximated and where, what is unresolved, and what the next phase needs.

**Last updated:** 2026-09-21, end of Session E. CI green. 367 C++ tests (one
skipped, allowlisted), 213 Python.

---

## The result this project is for

A strategy in this repository measured a **Sharpe of 2.414**, above the **1.927**
threshold that a 1,512-run null sweep produced as the bar a result must clear
before it can be called distinguishable from noise.

It was an artefact. Every one of its fills — **56 of 56** on one fixture and
**55 of 55** on the other — had taken its price from the very bar its own
decision was made on. Corrected, the same strategy measures **1.810**, below the
threshold.

**The apparent result was the lookahead.** The effect was worth 0.60 of Sharpe
here and it was the whole distance between "this looks real" and "this does not".

Three instruments caught it, and none of them was looking for it:

| | |
|---|---|
| **The bit-exact parity assertion** | Two independent implementations of the same strategy, compared with no tolerance. They diverged by $606.07 at session 1 of 126 on one symbol, which is a number small enough to have been waved away by any tolerance at all. |
| **The per-fill timestamp triple** | Decision horizon, price timestamp, fill timestamp, recorded on every fill. It turned an argument about the engine into a count: zero honest fills in either run. |
| **The empirical null from M8** | Without a measured threshold, 2.414 and 1.810 are both just numbers. The threshold is what makes the correction mean something, and it is why the sweep was worth 1,512 runs. |

The first of those three is duplication on purpose, and it is the one most likely
to be removed by someone tidying up: two implementations agreeing is evidence
only while they stay causally independent. Deleting either half, or giving them a
shared arithmetic core, leaves the assertion passing and measuring nothing. See
[`docs/AUDIT.md`](AUDIT.md) under "Deliberate duplication" for what that forbids.

**The 0.60 is not a constant.** It is one effect, on one small synthetic fixture,
under one rebalance rule. What transfers is the shape of the thing: a lookahead
does not announce itself as a bug, it announces itself as a *result*, and the
only defence is instruments built before you need them.

That is the artefact this project is handing over. It is a better thing to hand
someone than a result it found.

---

## Picking this up on another machine

Everything needed to build is committed. Two things are not, because they are
fetched rather than authored, and `data/cache/` is gitignored.

```powershell
# 1. vcpkg, if this machine does not have it. A FULL clone -- not --depth 1,
#    because vcpkg needs history to resolve versions.
git clone https://github.com/microsoft/vcpkg.git $env:USERPROFILE\vcpkg
& $env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat

# 2. Build. The first configure builds the dependency tree from source and
#    takes a while; later ones are cached.
./scripts/dev-env.ps1 cmake --preset release
./scripts/dev-env.ps1 cmake --build --preset release

# 3. Test. Through check_skips.py rather than ctest directly -- a skipped test
#    is not a pass, and this project has the scar to prove it.
./scripts/dev-env.ps1 python scripts/check_skips.py --preset release

# 4. The index membership history. This one still works.
python scripts/fetch_sp500.py
marketscan.exe --headless --build-universe data/cache/universe/sp500_history.csv

# 5. Price data. This one does NOT work, and is not going to -- see
#    docs/DATA_POLICY.md. The script is kept because it reports the gate
#    correctly, not because it gets past it.
python scripts/fetch_stooq.py --symbols AAPL

# 6. The research layer. Pure Python, no vcpkg, no MSVC, no engine.
pip install -r research/requirements.txt
pytest
```

Nothing else is machine-specific. The tests do not touch the network and the
committed fixtures are synthetic.

The research layer is worth knowing about separately: it needs none of the C++
toolchain. On a machine with Python and no vcpkg -- which is a normal state for
a machine that has only ever run the analysis -- `pytest` and
`python -m research.sweep` both work from a fresh clone.

**On this machine specifically:** Norton intercepts TLS and holds freshly built
binaries. Both need a SONAR/Auto-Protect exclusion, which is a different setting
from the scan exclusion, and the build still wants `-j 3` — twenty binaries
linked and launched at once will stall regardless.


---

## What exists

| | | |
|---|---|---|
| **Engine** | one event loop, templated on its clock | `EngineT<SimulatedClock>` and `EngineT<SystemClock>` both compiled and both run |
| **Money** | `int64` micro-dollars | the ledger identity holds exactly, not to a tolerance |
| **Plugin ABI** | pure C, frozen at v1 | loader branches on declared version; a v1 plugin survives a v2 host |
| **Execution** | spread-crossing by default, optional latency and partial fills | [`EXECUTION.md`](EXECUTION.md) |
| **Data** | daily bars, binary cache, one merged feed | [`DATA.md`](DATA.md) |
| **Identity** | `SecurityId`, dated resolution, dateless overload deleted | [`IDENTITY.md`](IDENTITY.md) |
| **Universe** | structural survivorship guard | a strategy has no API that returns a non-member |
| **GUI** | two view modes over one snapshot; thirteen panels in Operator | `docs/screenshots/overview.png`, `operator.png` |
| **Python** | pybind11 over the same `run_backtest` | `bindings/python/examples/quickstart.ipynb` |
| **Research** | synthetic panels, purged CV, constrained portfolios | [`research/README.md`](../research/README.md) |
| **Calibration** | the pipeline recovers a planted signal, is fooled by injected biases, and has a measured false-discovery rate | [`PATHOLOGIES.md`](PATHOLOGIES.md), [`FALSE_DISCOVERY.md`](FALSE_DISCOVERY.md) |
| **Thresholds** | every statistic judged against this pipeline's own null, not a textbook one | `research/thresholds.py` |

Milestones M0–M9 of Spec 1, and M0–M8b of Spec 2, are done. Specs are in
[`specs/`](specs/).

**327 C++ tests** on `debug`, `release`, `ci`, `ci-debug`; **328** on `full`.
**139 Python tests** in `research/tests`, run by the `research` CI job on
Linux -- deliberately, because the research layer depends on nothing in the C++
toolchain and the first import that changes that turns the job red.

---

---

## What is proven, and by which measurement

Nothing in this column rests on an argument. Each row names the assertion that
holds it, and every one of them runs in CI.

| Claim | Measurement |
|---|---|
| **The ledger cannot create or destroy value.** | `equity == starting + realized - commission + unrealized`, exactly, in integer micro-dollars. Checked after every fill, including across the delisting path. |
| **Two independent implementations of one strategy agree to the micro-dollar.** | `tests/test_parity.cpp`, no tolerance parameter anywhere. A C++ theory and a Python reference over the same committed bytes. |
| **No fill uses a price its decision had already seen.** | The per-fill timestamp triple, with `price_ts > decision_ts` asserted on both fixtures. Forced exits excluded, and the exclusion is named rather than quiet. |
| **Position sizing is bit-identical across the two layers.** | 54 adversarial cases in `data/samples/sizing_cases.csv`, read by both suites, asserted equal. |
| **A strategy is never shown a bar for a non-member.** | Structural: `StrategyInstance` is the only path from an event to a plugin, and the host context exposes no way to ask about a non-member. 336 events withheld on the committed synthetic universe, counted rather than claimed. |
| **A search of 252 variants over three years needs a Sharpe of 1.927 to mean anything.** | 1,512 null runs, Gumbel fit, effective-tries calibration. `docs/FALSE_DISCOVERY.md`. |
| **A lookahead is worth more than it looks.** | 2.414 → 1.810 on the churn fixture. See the top of this file. |
| **Assuming a zero delisting return flatters a backtest.** | 29.42 points of return and 3.14 of Sharpe, measured by running one strategy under both settings of `DelistingReturnModel`. Measured independently in the research layer as well, on its own panel. |
| **The two view modes cannot disagree.** | `tests/test_view_modes.cpp`: same series, same arrays, same entity colours across densities; hiding a series repaints nothing. |
| **No test skips silently.** | `scripts/check_skips.py` enumerates every skip and fails on any not allowlisted with a written reason. |
| **The same plugin binary makes the same decisions live and in replay, *including when it reacts to its own orders*.** | `ms_live equivalence`, regular session, 2026-09-23: 70,883 live events, 70,883 replayed, **128 decisions identical** field by field, with 128 of 128 submissions matched against the recording and 434 of 434 venue responses replayed onto the right orders. 66 of those decisions followed a partial fill. Supersedes the 2026-09-21 result (10 decisions, market data only, strategy written not to look at its own orders). Not in CI -- it needs a venue and an open market. See [`EXECUTION.md`](EXECUTION.md). |
| **A replay hands a strategy what the venue handed it, not a simulation of it.** | A recording carries submissions and responses as well as market data, and `RecordedBroker` replays them through the same `OrderTracker` the live broker uses. The Nth submission is asserted to correspond before its recorded answer is delivered, and a response matching no order is a fatal misroute. `tests/test_recorded_broker.cpp`, no network. |
| **A live ledger divergence is reported, never corrected.** | Seen doing it: a run reported 6 shares against the venue's 9 and the gap traced to an earlier run that died holding three. Both directions of the comparison are asserted offline in `tests/test_live_broker.cpp`, including that "could not check" is not "they agree". |
| **An order survives losing its connection.** | The client order id is derived from the engine order id before anything is sent, so a reconnect reclaims by a string this process chose. Exercised for real: connection dropped mid-session, 4 orders reclaimed, 3 stale updates dropped rather than re-delivered. |
| **A v1 plugin still loads under ABI v2.** | `tests/test_abi_v2.cpp`, with a fixture that declares version 1 by hand, trades, and is asserted never to receive the tenth callback. |

---

## What is approximated, and where

Each of these is a substitution for a measurement this project does not have.
They are named in code, not just in documentation, so a result carries its
assumption with it.

### The assumed exit

A held position in a name that leaves the index is liquidated at the last price
the book had. `DelistingReturnModel` names that assumption on both sides and the
alternative beside it; `last_price` is the default, so no committed number moved
when the switch appeared.

**Worth 29.42 points of return and 3.14 of Sharpe** on the churn fixture — enough
to flip its sign. On real data `apply_true_return` has nothing to apply, so every
number over a universe with departures in it carries `last_price`. CRSP deletes
this.

### Identity inference

Forty-one splits of a ticker into two securities, **every one of them inferred
from a 730-day threshold and none checked against a price series.** Over-splitting
is the safe direction — it costs history rather than fabricating a series — and
that is a real defence, not a claim of correctness. See below.

### The fill model

A fill is the next close. A real order sent after a close fills *somewhere in*
the next session and nothing here models where. There is no queue position, no
partial-fill ladder, no borrow cost and no locate. Market impact is a square-root
law with a coefficient nobody measured.

### The research layer is frictionless

By design: the engine owns execution, and a second cost model would be a second
answer to a question that already has one. Every Sharpe in `research/` reads
better than anything tradeable.

### The null is Gaussian

The false-discovery threshold was measured against a market factor plus normal
noise. Real returns have fat tails, volatility clustering and cross-sectional
structure that panel does not contain, all of which give a search *more* to find.
**The measured threshold is a floor.**

---

## What is unresolved

| | |
|---|---|
| **M3b has never run.** | Zero of 53 gaps measured, zero of 10 ground-truth cases scored. CRSP-dependent, not blocked — see below. |
| **Forty-one splits want a human.** | The threshold decided them; a person should review the ones it nearly went the other way on. Four overrides exist and are hand-written company facts, not measurements. |
| **One transient test failure, unexplained.** | Seen once, never reproduced. Recorded rather than closed. |
| **No real market data has entered the *research* pipeline.** | Corrected in Session F, because the old wording became false. Real IEX quotes, trades and bars now reach the engine through `LiveMarketDataFeed`, and real orders reach a paper venue. But no committed result, table or figure rests on any of it: the recordings are deleted, nothing is fitted to them, and every number in the research layer still comes from synthetic prices. Not one *conclusion* in this repository is a statement about markets. |
| ~~**Partial fills were never exercised against a venue.**~~ | **Closed 2026-09-23.** Orders are sized from a notional target rather than being one share, and a regular-session run produced **66 partial fills** against the paper venue, replayed onto the right orders. |
| **The equivalence test is not in CI and cannot be.** | It needs a venue, credentials and an open market. It is run by hand and its result is quoted with its date in [`EXECUTION.md`](EXECUTION.md). A result nobody can reproduce on demand is weaker than one CI re-runs nightly, and that is the honest standing of this one. |
| ~~**Extended hours were doing the work.**~~ | **Closed 2026-09-23.** Three 300-second regular-session runs, five symbols, all five carrying events: 70,883 events in the run that produced the equivalence result, against 463 for the extended-hours session it supersedes. |
| ~~**A recording held only market data.**~~ | **Opened and closed 2026-09-23.** This is what made the first regular-session run diverge: replay fed the strategy simulated acceptances where live had 27 refusals, so the two reacted correctly to *different information* and the test blamed the strategy. Recordings now carry both directions. The wrong conclusion drawn from that run -- that a theory must be a pure function of market data -- was never implemented, and would have outlawed every realistic strategy this platform exists to run. |
| **The rejection bait is confirmed in isolation and not in place.** | `ms_live bait` sends each candidate to the venue alone, and on 2026-09-23 all five were refused in a regular session -- a sub-penny limit price earns `422 sub-penny increment does not fulfill minimum pricing criteria`. But the bait still produced no identifiable refusal inside the integrated run, and whether it was sent and accepted or skipped for want of a third quoting instrument is not determinable from the run's output. Confirmed in isolation is not confirmed in place. The four refusals that did occur came from the resting leg selling more than it held. |
| **Both legs alternate and the position still drifts.** | The run finished 81 shares of QQQ short, $60,030, inside the $150,000 exposure cap but not flat. Alternating bounds the drift; it does not remove it, because partial fills do not offset in pairs. |
| **The event-rate model has no figure for the open.** | Measured at 11:28, 14:04 and 14:11 ET: 342, 220 and 236 events per second across five symbols. 09:30-10:00 is expected to run hotter than any of those and has not been sampled. The previous model was 2.4x low precisely because mid-afternoon rates were treated as typical. |

---

## What the next phase needs

**Data, and it is not close.** This is the conclusion Session C Part 2 measured
rather than assumed, and it decides what is worth doing next.

The false-discovery bar is the spread of null Sharpes times an extreme-value
factor. The spread falls as `1/sqrt(sample length)`. The factor grows as roughly
`sqrt(2 ln n)` in the number of independent tries. Square root against logarithm:

| sample | bar, 25 variants | bar, 252 variants |
|---|---|---|
| 3 years | 1.78 | 2.03 |
| 8 years | 1.15 | 1.34 |
| 20 years | **0.68** | **0.77** |

**Twenty years with the full sweep gives a more usable bar than three years with
a tenth of the variants.** Discipline about what you test is worth something;
sample length is worth more, and it is not close. A realistic equity factor
Sharpe of 0.3–0.8 is not detectable *at all* on three years, at any level of
restraint about the size of the search.

That makes the data source a **research-design constraint rather than a
convenience**, and it is why CRSP back to 1926 matters more than any modelling
improvement currently available:

1. **It makes results possible at all.** Long samples move the bar below the
   effect size a real factor has.
2. **It deletes the identity heuristic.** PERMNO *is* the identity. The 730-day
   threshold, the 41 inferences and the whole of M3b are removed rather than
   validated.
3. **It deletes the assumed exit.** CRSP carries measured delisting returns, so
   `apply_true_return` stops being a synthetic-only setting.
4. **It carries delisted names**, which is the binding constraint no free source
   and no free tier satisfies. A vendor with only live tickers cannot answer these
   questions at any price.

Before any of that: `docs/DATA_POLICY.md` §4 governs what happens when licensed
data arrives — extracts outside the repository, cache gitignored, published
results aggregate rather than reconstructible. It is written while the question
is hypothetical so that it is already settled when it is not.

**What does not need doing:** more modelling. The engine, the research pipeline,
the guards and the instruments are built and measured. Pointing them at three
years of free daily bars would produce more numbers and no more knowledge.

---

## How it got here

The rest of this file is the record: what each session changed, what it cost in
committed numbers, and which assertion caught each mistake. It is kept because
several of the corrections are more instructive than the code that resulted, and
because a number that moved without an explanation is a number nobody can trust.

## What Session B measured

The three numbers the research harness exists to produce. All of them are
statements about this pipeline on synthetic data; none of them says anything
about markets.

| | | |
|---|---|---|
| **Recovery** | 18 runs across six values of λ, every one within two standard errors of the truth | [`figures/lambda_recovery.png`](figures/lambda_recovery.png) |
| **Pathologies** | survivorship 1.32× on the premium and +4.8%/yr on the index level; a lookahead R² of 0.66 against an honest 0.0002; a naive delisting policy removing 82% of the average loss | [`PATHOLOGIES.md`](PATHOLOGIES.md) |
| **False discovery** | **a Sharpe of 1.96** is the bar a result from a 252-variant search over three years has to clear to be distinguishable from noise | [`FALSE_DISCOVERY.md`](FALSE_DISCOVERY.md) |

Three of these changed after the first run produced something unexpected, and
the unexpected thing was right each time:

1. **Survivorship bias deflated the premium** in the first configuration. Not a
   bug: the failures were in the strategy's *short* leg, so removing them
   removed gains. The sign of survivorship bias follows which leg failed. Only
   the index-level inflation has a fixed sign, and the report says so instead of
   reporting the expected answer.
2. **The null panel was not null.** Its departure hazard was tilted by the
   exposure, which makes the exposure predict returns through the delisting
   channel — measurably, across seeds. A false-discovery rate measured on that
   would have been inflated by a signal that was genuinely there. The first
   40-minute sweep was discarded and re-run untilted; a test and its control
   pin it now.
3. **The best-of-N percentiles were not estimable** by resampling: drawing 252
   variants from a panel holding 252 returns the panel maximum every time, so
   the "95th percentile" was the largest Sharpe in the sweep. The threshold is
   now built from the per-variant distribution (1512 runs, near-normal) with the
   effective number of independent tries calibrated from the observed mean best
   — 252 correlated variants behave like 81 independent ones.

### The information coefficient, calibrated rather than trusted

M8 found that the IC's t-statistic is not calibrated and fails in the flattering
direction. M8b measured what to use instead, rather than trying to repair the
estimator: a better HAC correction is fiddly, uncertain at this sample size, and
would produce a number nobody could check.

So the IC was calibrated the way the Sharpe bar was — point the null at it,
across six horizons, 1512 runs:

| horizon | what a t of 1.96 demands | how often that fires on nothing | measured bar |
|---|---|---|---|
| 1 | 0.0074 | **8.3%** | 0.0082 |
| 2 | 0.0084 | **12.3%** | 0.0103 |
| 5 | 0.0117 | **17.1%** | 0.0156 |
| 10 | 0.0150 | **20.6%** | 0.0212 |
| 20 | 0.0180 | **25.8%** | 0.0315 |
| 40 | 0.0220 | **21.8%** | 0.0372 |

The nominal rule is anti-conservative at every horizon measured, and the gap
widens with the horizon: at twenty periods it demands an |IC| of
0.0180 where the null produces 0.0315
one time in twenty.

The bars are exported from `research/thresholds.py` beside the Sharpe bar, with
the same interface. The t-statistic is still computed — it is a serviceable
screen — and its measured false-positive rate now travels with it to the call
site: `PipelineResult.report()` prints the rate for its own horizon next to the
t, and `ic_bar()` and `ic_is_distinguishable()` answer against the measurement.
A horizon outside the calibrated range returns NaN rather than an extrapolation.


---

## M9 parity: exact, and what it took

The same strategy — long the top decile of the synthetic exposure, short the
bottom, monthly — written twice: `theories/decile_parity/` as a C++ theory on
`CrossSectionalStrategy`, and `research/parity.py` as a second implementation of
the **engine's** accounting rather than of `portfolio.py`'s.

**The equity curves are now identical.** Asserted as equality in
`tests/test_parity.cpp`, with no tolerance parameter anywhere, and green in CI on
both presets. Getting there took two fixes, and neither was the one that looked
likely at the start.

### One: the engine sized from a price it could not have known

The C++ side sized from `equity()`. The ledger marks each position at the last
price its book holds, and `on_session_close` fires while the engine is
dispatching the first bar of the *next* session — which it has already applied.
So one symbol, whichever bar rolled the date, was marked a day ahead at the
moment the decision was taken.

That was a disagreement between the layers and a small lookahead at once. The fix
is [`SIZING.md`](SIZING.md): the rule written down, with decision-time equity
computed from the cross-section the decision is based on. Both layers implement
that page independently — `include/marketscan/strategy/sizing.hpp` and
`research/sizing.py`, neither calling the other — and a committed grid of 54
adversarial cases asserts they produce bit-identical share counts.

### Two: the two layers snapshotted different instants

What remained was one session in 126, one micro-dollar, with identical final
equity and a $766 cash gap — which says the positions are equivalent and the
difference heals. The engine holds the *previous* book through all of session N
and changes it while N+1's first bar is dispatched. It trades at session N's
closes either way, so the economics always agreed; the reference was simply
recording post-trade where the engine records pre-trade.

The reference now records pre-trade. Five rows of the committed curve moved: cash
shifted back one rebalance on all five, equity on exactly one, by one
micro-dollar, onto the number the engine had been reporting throughout.

### What this cost in committed numbers

Fix one: one micro-dollar, on one session, in `data/samples/parity_expected.csv`,
caught by the equality assertion itself. Fix two: five rows of cash and one of
equity, same file. Fix three moved every row of every parity curve — the table is
in *the assumed instant*.

### Churn parity, added in Session D Part 2

A second fixture, `data/samples/parity_churn_*`, where names leave: 29
departures, a membership history the engine builds a `Universe` from, and a
delisting return table the generator knows the truth of. Both layers are asserted
identical **under each setting of `DelistingReturnModel`**, which is what splits
agreement about the *timing* of a forced exit from agreement about its
*magnitude* — the distinction the old fixture could not make, because the
departure return used to be the departing name's final bar. See *What Part 3
owed* below.

### Three: the fill used the price the decision was made from

Session D Part 3, and churn parity is what caught it. Both layers filled a
decision taken from session N's close *at* session N's close, for every symbol but
the one whose bar rolled the session. Measured with the triple, before anything was
changed: **56 of 56 fills on the churn-free fixture and 55 of 55 on the churn
fixture had `decision_ts == price_ts`.** Not one honest fill in either run.

`CrossSectionalStrategy` now states a target book and each name's target becomes an
order when that name next prints, so every fill lands at the first price its symbol
printed after the decision. Both layers do it and every committed curve moved; see
*the assumed instant* in [`EXECUTION.md`](EXECUTION.md) for the table, and *What
Part 3 owed* below for what it cost.

---

## Session D: where it stands

| | | |
|---|---|---|
| 0 | Housekeeping, reboot, baseline | **done** — baseline below |
| 1 | Bit-identical sizing | **done**, CI green |
| 2 | Make delisting returns real | **done** — both layers, churn parity exact |
| 3 | The lookahead probe | **done** — triple recorded, invariant asserted, fill-timing lookahead found and fixed |
| 4 | Toolchain gate (GUI stack) | **passed** — vswhere 17.14.37710.0, full GUI build from source |
| 5 | Universe Inspector | **done** — panel, snapshot, five tests, screenshot committed |
| 6–7 | Dual view, close-out | carried into Session E |

## Session E: where it stands

| | | |
|---|---|---|
| 0 | Verify Session D Part 2 | **verified**, nothing missing — see below |
| 1 | `DATA_POLICY.md` | **done** |
| 2 | Reclassify M3b, label identity honestly | **done** |
| 3 | Make skips loud | **done** — and it found a worse bug than the skip |
| 4 | The dual view | **done** |
| 5 | Close-out | **done** |

### Part 0: Session D Part 2, verified

Checked item by item rather than taken on trust, because it was never reported:

| | |
|---|---|
| Generator emits a delisting return separate from the final bar, bimodal by cause | Present. Worst final bar among departures −3.9%; worst delisting return −100%. `PanelSpec.delist_return` is gone. |
| Both layers carry the named model, `last_price` default | Present. `research/delisting.py` and `include/marketscan/exec/delisting.hpp`, spelled identically. |
| `PATHOLOGIES.md` carries the flattery measurement | Present. §4: +54.64% against +32.84% total return, +4.30 against +1.91 Sharpe. |
| Churn parity asserted under `apply_true_return` | Present. Exact equality, plus every booked adjustment reconciled row for row. |

Nothing needed building.

### Part 3: making skips loud found something worse than a skip

The brief was to stop a skipped test reading as a pass. Doing it surfaced a
failure one step further down.

The calibration test gated on `std::filesystem::exists("data/cache/stooq")`. That
directory got created during this session — empty, as a side effect of
regenerating the identity report. With it present the test no longer skipped: it
built the membership history, measured against an empty cache, found every case
unresolved, **made zero assertions, and reported green.** "0 verified, 10
coverage-limited", in a `WARN` nobody reads.

A skipped test at least announces itself. A vacuous pass does not, and this is
the test whose entire job is to say whether the identity classifier has ever been
right about anything.

Both halves are fixed:

- **The test gates on data, not on a directory.** If it verifies zero cases it
  `SKIP`s with the count in the reason, so the state is legible whether or not
  somebody has an empty folder lying around.
- **CI runs ctest through `scripts/check_skips.py`.** Every skip is named in the
  job summary with its reason; any skip not in `tests/allowed_skips.txt` fails the
  job. The allowlist requires a written justification per entry and the parser
  refuses one without it.

`ctest --output-junit` turned out to be useless for this: a run with 351 passes
and one skip writes 351 testcases, every one `status="run"`, and `skipped="0"` on
the suite. Trusting it would have rebuilt the exact blindness the script exists to
remove, so the skips are parsed from ctest's console block instead. Ten tests
cover the parser and the allowlist.

**The one allowlisted skip** is the identity calibration case, with the
justification that it is CRSP-dependent — see above. It is the only entry, and
deleting it is the acceptance test for a vendor with delisted coverage.


### Part 0: the baseline, and what it cost to get

Local runs now work. The cause was never scanning — it was **SONAR /
Auto-Protect**, a separate exclusion from the scan exclusion, holding each
freshly built binary in a kernel-mode wait: one thread, zero CPU, surviving
`Kill()`. A reboot cleared the stale handles but not the interception; the
Auto-Protect exclusion fixed it.

One more thing had to be learned after that. With the exclusion in place, a build
that links 21 test binaries and then runs all 21 through `catch_discover_tests`
in parallel still hangs — every one of the 21 stalls, while the same binary run on
its own returns immediately. Building with `-j 3` is enough.

Baseline, taken before anything in Part 2 was changed:

| | |
|---|---|
| C++ (`ci` configuration: Release, `MARKETSCAN_WERROR=ON`) | **334 passed, 0 failed, 1 skipped** |
| Python (`research/tests`) | **185 passed** |
| CI on `main` | green at `cb4aefb` |

**The one skipped test is not cosmetic.** It is the identity classifier's
calibration case, and it has skipped in every run this file reports because the
price data it needs has never been fetched — so the measurement that was supposed
to resolve forty-one identity splits has never executed. A green suite with a
skip in it is how that stayed invisible. See *M3b is CRSP-dependent, not blocked on
Stooq*, below.

Parts 2 and 3 then moved them, and the movement is accounted for:

| | C++ | Python |
|---|---|---|
| baseline | 334 | 185 |
| after Part 2 | 343 (+9) | 199 (+14) |
| after Part 3 | 346 (+3) | 202 (+3) |
| after Part 5 | 351 (+5) | 203 (+1) |
| after Session E Part 3 | 351, **1 skipped and allowlisted** | 213 (+10) |
| after Session E Part 4 | 367 (+16) | 213 |

Part 2's additions are all in `test_parity.cpp` — the churn fixture, the two curve
comparisons, the row-for-row reconciliation, the flattery gate, the rolling-symbol
property and the two loader cases. Part 3's are the invariant, the triple and the target
book's release-or-abandon behaviour on the C++ side, and the release instant, the
release order and abandonment on the Python side. `clang-format` 19.1.1 clean across the
tree throughout.

**Part 3 moved committed results**, which Session D said to expect: every parity
curve, and the two cost constants in `test_parity.cpp`. The numbers and the
assertion that caught them are in *What Part 3 owed* below. No test changed from
passing to failing.


### Part 4: the toolchain gate, passed

`vswhere` reports **17.14.37710.0** with `VC.Tools.x86.x64`. `cmake --preset
release` configures with `gui ON` and builds ImGui, ImPlot and GLFW from source
through vcpkg; the full build links 115 targets including `marketscan.exe` and
`glfw3.dll`, and `ctest --preset release` is **351 passed, 0 failed, 1 skipped** —
the same as the `ci` configuration, which is the point of running both.

### Part 5: the Universe Inspector

The survivorship guard was correct and invisible. It is now visible.

`SessionConfig` takes a membership history and `--universe` reaches the GUI, so
the live session builds a `Universe` and a `SecurityMaster` and installs them on
every strategy — which it could not do before; only the headless path could.
`Session::universe_snapshot_locked` assembles what the panel draws, once per
publish rather than per frame, and **the panel computes nothing**. That is the
same rule `SPEC_VIEW_MODES.md` sets for the dual view and it is here for the same
reason: a panel that derived its own member list would eventually disagree with
the guard it exists to show, and it would disagree in a screenshot first.

Four tabs: members as of the session clock, every add and drop over the whole
history, the forced exits that actually happened, and the identity decisions with
their provenance — override, threshold, near miss — plus what the price series
said and whether it contradicts the threshold.

Two things the panel refuses to do quietly. A session with **no** universe says so
in as many words rather than going blank, because a run without one is correct for
a single instrument and survivorship-unsafe for a cross-section and the difference
has to be visible. And a universe file that cannot be read **fails the session**:
a guard that silently did not load is worse than no guard, because the output
looks identical and nothing in it says which one produced it.

![The Universe Inspector](screenshots/universe.png)

On the run above: 84 securities over 84 membership periods, 500 snapshot dates,
**336 events withheld** from the strategies and 2 forced exits. The withheld count
is the guard as a number rather than a claim — a run where it stays at zero on a
universe that churns is a run that was never filtered, and the panel colours it as
a warning.

**Identity provenance is present and empty on this universe, which is honest
rather than broken.** No ticker in a synthetic history has a gap in its
membership, so the split heuristic has nothing to decide; the panel says that
instead of showing an empty table.

It is worth being exact about what that means, because the panel can only show
what exists: on the real S&P history the provenance column would read
`threshold` for forty-nine gaps and `override` for four, and the measurement
column would read `not measured` for all fifty-three. The panel is not waiting on
a feature. It is waiting on a measurement that has never run — see *Stooq is
gated, and M3b has therefore never run*, below.

### Screenshots capture themselves now

`scripts/capture_screenshot.ps1` read pixels off the desktop, which needs the
window genuinely in front. Windows refuses `SetForegroundWindow` to a process
that is not already foreground, so run from a script rather than by a person
clicking, it produced a **white rectangle and wrote it out as though it were a
screenshot**. `PrintWindow` does not rescue it either: an OpenGL window has no DWM
redirection surface to hand over while it is behind something.

The application captures itself instead. `--screenshot <file>` replays for
`--screenshot-after` seconds, reads its own back buffer, writes a 24-bit BMP and
exits; the script converts it to PNG and **refuses a capture that comes back one
flat colour**. The window still has to exist and be rendering — this is not
headless — but it no longer has to be visible, in front, or unobstructed, which is
what makes a screenshot a command anybody can repeat.

`--focus <panel>` picks which panel is in front, so a screenshot of one view is
reproducible rather than arranged.


### Part 4: the dual view

Two presentations over one running engine, one keystroke apart (`F2`). Overview
asks *is this working, and is it beating the market?* and contains a hero figure,
one chart, one drawdown strip, four stat tiles and one selector. Operator is the
dense dashboard, unchanged. Neither is a lite version of the other.

![Overview](screenshots/overview.png)

The spec's three rules are the ones that erode under implementation pressure, so
each is a test rather than an intention. What made that possible was a
refactoring: the colour table, the entity-to-colour assignment and the chart
**model** were moved into `marketscan_app`, the half of the application that does
not link ImGui. What each mode would draw can therefore be built and compared
without a window, a GPU or a screenshot.

| Rule | How it is held |
|---|---|
| One snapshot, two renderers, zero duplicate computation | Every figure is a field. `excess_return` was added to `StrategySnapshot` rather than subtracted in the view — it is *one subtraction*, which is exactly why it was tempting. |
| One `EquityChart` at two densities | `build_chart_model` is pure and both modes call it. A test asserts the two produce the same series, pointing at the same arrays, with the same entity colours — differing only in line weight, greying and labels. |
| Colour follows the entity, never draw order | `SeriesColors` assigns slots from sorted stable ids, built from **every** theory including hidden ones. A test hides a series and asserts the survivors are unrepainted. |

Eight categorical slots and then fold: a ninth theory goes to "Other" in gray
rather than getting a generated hue, because a ninth colour is indistinguishable
from one of the eight for a colour-blind viewer — it would counterfeit an
identity rather than add one.

**Five panels were still colouring by index** — leaderboard, blotter, drawdown,
relative strength and theory detail — which is the precise failure the spec names:
hide one series and every series after it repaints, so the leaderboard stops
matching the chart. All five now colour by entity.

**Two bugs the tests and the screenshots caught:**

- The benchmark's hue was borrowed from the muted ink, which happens to be the
  same value as the de-emphasis gray. The benchmark therefore rendered
  *identically to the context series it is supposed to be the reference for*. It
  has its own pinned token now, and a test asserts it differs from the context
  gray and from all eight slots.
- Overview's hero figure read **`-+13.06%`**. The view had grown its own
  signed-percentage helper on top of `format_percent`, which already formats with
  `%+`. The spec's requirement is that a P&L always carries a sign, not that every
  view implements its own signing — one shared formatter satisfies it and a second
  one is how two views end up disagreeing about how a number looks.

No dual-axis chart exists anywhere: the chart model carries no second scale and
no field that could introduce one.


---

## The open items, in detail

The summary of these is above, under *What is unresolved*. This is the working
out: what was diagnosed, what it cost, and which options are actually available.

### 1. M3b is CRSP-dependent, not blocked on Stooq

**This was misfiled, and the misfiling cost sessions.** M3b has been recorded as
*blocked on Stooq* — as though one vendor came back and the milestone resumed.
That is wrong, and it invited the wrong kind of work: hunting for a way through a
gate, or waiting for it to lift.

M3b measures **price continuity across a membership gap**, for securities that
may have stopped trading decades ago. The input it needs is the price history of
dead tickers. Free vendors do not carry that — not Stooq, and not the free tiers
of the vendors that would replace it. A source carrying only currently-listed
names cannot answer this question **at any price**, because the names the
question is about are exactly the ones it is missing.

So M3b is not a free-data milestone and not an engineering problem. It is
**CRSP-dependent**, and it waits for data rather than for effort. CRSP settles it
by deleting it: PERMNO *is* the identity, and the entire inference heuristic gets
removed rather than validated.

That reclassification is the point. What follows is why the free path closed.

**The gate, diagnosed rather than inferred.** Probed 2026-09-21 from this
machine:

| | |
|---|---|
| DNS | resolves to `159.69.202.225`, `78.47.47.99` — Hetzner, Stooq's own hosting |
| TCP + TLS | completes, TLS 1.3 |
| HTTP | **200**, `Server: openresty`, `Cache-Control: no-store`, 796 bytes of HTML |
| Rate-limit signals | none: no `429`, no `Retry-After` |
| CDN | no `CF-RAY` — this is Stooq's own edge, not Cloudflare |

The body is a **proof-of-work challenge**: find `n` such that `SHA-256(c + n)`
begins with four hex zeros, then `POST /__verify`. It is served unconditionally
on a first request from a cold connection, which is what rules out rate
limiting — a limiter does not challenge you before you have asked for anything.

So it is **a deliberate change on Stooq's end**, not throttling, not a network
policy here, and not the endpoint agent.

**This is not a bug to route around.** The challenge is trivially solvable in
a few lines — four hex zeros is about sixty-five thousand hashes, milliseconds
of work. Nothing here does it. Spec 1 forbids it, and more to the point a
proof-of-work gate is an unambiguous statement by the operator that automated
clients are not welcome. That also settles the "wait and see" option below:
gates like this are a decision, and decisions do not lapse.

*Incidental, and worth knowing before anyone else diagnoses a TLS problem on
this machine:* Norton is intercepting TLS here — the chain presents
`Norton Web/Mail Shield Root` for `*.stooq.com`. It is **not** the blocker; it
passed Stooq's real response through intact. It is the same endpoint agent that
holds freshly built binaries, and it will make any certificate-level diagnosis
on this machine misleading unless you expect it.

**The consequence, stated plainly: M3b has never been run on real data, and the
identity model rests on the 730-day threshold alone.**

| | |
|---|---|
| Gaps considered | 53 |
| Resolved by measurement | **0** |
| Governed by the threshold | 49 |
| Governed by a hand-written override | 4 |
| Splits made | 41, **all of them inferences** |
| Ground-truth cases ever scored | **0 of 10** |

`data/cache/` has never held real price data. Every gap in
[`IDENTITY_DECISIONS.md`](IDENTITY_DECISIONS.md) reads `no price data for this
ticker`, without exception, and `measure_identities` has never had anything to
measure.

The calibration test — *the classifier agrees with the cases we are sure of* —
**skips**, and it is the one skipped test in every count this file reports. A
green suite has been carrying it the whole time, which is exactly how an unrun
measurement stays invisible.

Two details that make this worse rather than better:

- **The 4 overrides are not measurements.** They are hand-written company facts,
  and `write_measured_overrides` — which writes rows from measurements only — has
  never had a measurement to write. So four gaps rest on a human's assertion and
  forty-nine on a threshold nobody has checked against reality.
- **The report cannot currently be regenerated.** `data/cache/universe/sp500_history.csv`
  is gone too, so `IDENTITY_DECISIONS.md` is a committed artefact whose input no
  longer exists on disk. The membership half is recoverable —
  `scripts/fetch_sp500.py` reads the fja05680/sp500 project on GitHub, which is
  not gated. The price half is not.

**What the classifier is, precisely:** built, unit-tested against committed
synthetic fixtures, and **never validated against a single real case.** The ten
labelled cases in `data/identity_calibration.csv` include the two traps the whole
design exists for — `HP` is Helmerich & Payne and not Hewlett-Packard; `CCI` was
Citicorp before Crown Castle took the ticker — and neither has ever been scored.

Everything else works without it. The cache format, the feed, the universe, the
guard and the classifier's own logic are exercised against committed synthetic
fixtures, and the Universe Inspector draws real membership from them.

**The options, none of which should be picked without deciding:**

| | |
|---|---|
| Wait | Now looks wrong. A proof-of-work gate is a decision by the operator, not an outage, and there is no reason to expect it to lapse. |
| Another free source | Would restore *a* price feed, and would **not** restore M3b. Delisted coverage is the binding constraint and free tiers do not have it. Tiingo and EODHD both grant API access in their terms, which clears [`DATA_POLICY.md`](DATA_POLICY.md) §3 — but the question to ask either is "do you carry names that are no longer listed", and if the answer is no, the money buys nothing this milestone needs. |
| A licensed source | CRSP deletes this problem entirely — PERMNO *is* the identity, and the whole inference heuristic gets deleted rather than adapted. It also deletes the assumed exit. **This is the only option that actually finishes M3b.** |
| Ship without it | Legitimate, and it is what has happened so far. The splits stay inferences, the report says so, and over-splitting is the safe direction. What is **not** legitimate is letting the skipped test read as a passing suite. |

**Where this is written down now.** The unvalidated status is stated at the top of
[`IDENTITY_DECISIONS.md`](IDENTITY_DECISIONS.md) — generated from the measured
count, so the sentence changes the day the count does — in the Universe
Inspector's Identity tab, and in [`IDENTITY.md`](IDENTITY.md), which previously
said the classifier was "calibrated by" the ten known cases. It is not: those
cases have never been scored. That word is gone.

The policy that governs the source decision is
[`DATA_POLICY.md`](DATA_POLICY.md).

### 2. Forty-one identity splits want a human

`docs/IDENTITY_DECISIONS.md` has the table. Roughly a quarter are probably
wrong in the `keep` direction — continuously-listed companies that merely left
the index. Four obvious ones are already overridden (`AMD`, `CIEN`, `FSLR`,
`FMC`).

Corrections go in `data/security_overrides.csv`, one line each, then re-run
`--build-universe`. **Note:** if measurement ever becomes possible, that file is
regenerated from measurements and hand-written rows are overwritten — keep
durable corrections somewhere else and merge.

### 3. One unidentified transient test failure

A single `full`-preset run failed one test; three subsequent runs were clean and
the failing test was not captured. Not reproduced, not diagnosed. If it recurs,
grab the name — an intermittent failure nobody can name is worse than one that
fails every time.

---

## What each session owed, and what it delivered

**Session C is part-done.** Parts 0–3 of `MARKETSCAN_SESSION_C.md` are complete
except where noted; Parts 4–6 have not been started, which is the boundary that
document itself nominates as clean.

| | | |
|---|---|---|
| 0 | Commit Session B | done — five milestone commits, CI green |
| 1 | M8b, empirical IC thresholds | done |
| 2 | What moves the bar | done |
| 3 | M9 parity | done — churn-free parity, the cost check, and (in Session D Part 2) **churn parity, exact under both delisting models** |
| 4 | Universe Inspector panel | done in Session D Part 5 |
| 5 | M9b, the dual view | done in Session E Part 4 |
| 6 | Close-out: README and this file | done in Session E Part 5 |

### The identity model, and what is owed to the next phase

**M3b — the price-continuity measurement built to resolve the forty-one identity
splits — has never executed on real data.** Not deferred, not partially done:
zero of fifty-three gaps measured, zero of ten ground-truth cases scored.

It is now filed as **CRSP-dependent** rather than blocked, because that is what it
is: the measurement needs delisted price history, and no source this project may
use carries it. See *M3b is CRSP-dependent* above. Waiting is not a plan and
neither is a free substitute.

What that leaves standing: the 730-day threshold governs all 41 splits,
over-splitting is the safe direction, and every artefact that reports an identity
decision now says so on its face rather than implying it. That is a defensible
place to ship from. It is not the same thing as being right, and the README says
so too.

### What Part 3 owed, and what it got

**Churn parity: run, and exact under both delisting models.** Session D Part 2
made it possible and then it passed. The fixture is
`data/samples/parity_churn_*` — thirty names over six months, 29 departures, a
membership history the engine builds a `Universe` from, and a delisting return
table the generator knows the truth of.

| | |
|---|---|
| `last_price` | engine and reference agree to the micro-dollar across 126 sessions |
| `apply_true_return` | same, and every booked adjustment reconciled row for row — name, date, position, mark, return, micro-dollars |
| held departures | 7, across both causes, including one total loss and one short whose name left at a premium |

The earlier warning in this file was right and is now discharged rather than
repeated. It said to expect the churn case to measure *the timing of the exit, not
the size of the assumed one*, because the departure return **was** the departing
name's final bar — so "liquidate at the last price the book has" captured it by
construction and the two layers could agree perfectly while being wrong in the
same direction. Part 2 moved the delisting return out of the price series, which
splits the two questions:

- under `last_price` the adjustment is zero, so agreement is agreement about
  **when** each layer noticed a departure and at what price it liquidated;
- under `apply_true_return` both layers apply the same known return, so agreement
  is additionally agreement about **how much** the exit was worth;
- and the two runs are required to differ materially, so neither passes by the
  switch having done nothing.

What the assumption is worth on that fixture: **29.42 points of return and 3.14 of
Sharpe** (+8.63% and +1.81 assuming a zero delisting return, −20.78% and −1.33
applying it). It does not move the result, it flips its sign. Full detail in **the
assumed exit** in `EXECUTION.md`.

(Those are post-Part-3 figures. Before the fill-timing fix they were 30.10 points
and 3.55 of Sharpe, on a `last_price` Sharpe of +2.41 — see below.)

**The one-symbol lookahead: observed, measured, and fixed.** This file previously
said it was *predicted and not observed*, and that an inference from a curve that
happened to agree is not an answer. Both halves of that are now discharged, and it
was churn parity that caught it rather than a probe built to look for it.

The reading was correct and incomplete. `Engine::step` applies an event to the
book before dispatching it, so a strategy reacting to a print and the fill model
pricing its order see the same market — right for that case. But
`IStrategy::advance_session` fires `on_session_close(N)` from inside the dispatch
of the **first bar of session N+1**, so at the instant a session-N decision was
priced, exactly one symbol's book was already a session ahead.

What the reading missed is *which* fills that made wrong. The anomalous symbol was
the **only defensible one** — it filled a session after the decision. The other
twenty-nine filled at the close they were sized from, which is the larger error.

Two measurements, in the order they arrived.

*One, from churn parity.* On the churn fixture before it carried a dedicated roll
symbol, the short leg held SY0000, which sorts first on every session: sized
against a close of 148.6207 and filled at 143.216 — **$606.07 apart at session 1
of 126, growing to $942 by the end.** Caught by `tests/test_parity.cpp`'s "under
last_price the two layers agree about timing, exactly", which nobody wrote to look
for a lookahead.

*Two, from the triple.* With `decision_ts` and `price_ts` recorded per fill and
nothing else changed: **56 of 56 strategy fills on the churn-free fixture and 55
of 55 on the churn fixture were simultaneous** — `decision_ts == price_ts`, and
not one honest fill in either run.

Why the churn-free fixture never showed the first of those: SY0000 sorts first
there too and **never once lands in either decile**. That is luck, not a property
anybody chose, and the parity suite depended on it silently. It no longer does —
`the symbol that rolls each session is never in a traded leg` asserts it on both
fixtures, and the churn fixture satisfies it deliberately with a `ROLL` symbol
whose exposure is pinned mid-rank.

**The fix.** Not "fire the session-close callback earlier": `plugin_abi.h` is
frozen at v1 and its vtable has no session hook, so the shim can only learn a
session ended by being handed the next one's first bar. So
`CrossSectionalStrategy` carries a **target book** — `on_cross_section` states
what it wants to hold, and each name's target becomes an order when that name next
prints. Every fill lands at the first price its symbol printed after the decision,
uniformly, no symbol special, no ABI change. A target whose symbol never prints
again is abandoned rather than carried.

Two things went with it. The close-then-open ordering in `SIZING.md` is gone,
because releases happen in bar order and no strategy controls it; a strategy for
which cash binds has to hold gross back instead. And the reference's equity
snapshot moved from pre-trade to post-release, because a session's trades are now
the previous session's targets landing *inside* it.

**What it cost, with the assertion that caught it.**

| | equity before | after | Sharpe before | after |
|---|---|---|---|---|
| `parity_expected.csv` | $106,660.71 | $107,038.64 | +1.313 | +1.414 |
| churn, `last_price` | $111,739.45 | $108,633.80 | +2.414 | **+1.810** |
| churn, `apply_true_return` | $81,635.69 | $79,216.37 | −1.140 | −1.326 |

Plus the two cost constants in `test_parity.cpp`: spread $85.84 to $84.74, and
spread-plus-impact $1,136.02 to $1,114.27.

The sign is not uniform — the churn-free path got $377.92 *better* without the
lookahead. Trading a decile ranking a session later is a different strategy, not a
strictly worse one, and on one synthetic path the noise can go either way; the same
caveat as *A note on the numbers* in `EXECUTION.md`. What the lookahead bought was
not a uniform edge, it was a systematic misstatement.

**What is not ambiguous is the Sharpe on the churn fixture.** +2.414 cleared the
1.927 null-Sharpe threshold from the 1,512-run sweep; +1.810 does not. On this
fixture the lookahead was worth 0.60 of Sharpe, and it was the whole difference
between a rule that looks distinguishable from noise after adjusting for the size
of the search and one that does not. That is the closest thing this project has
produced to a demonstration of why the threshold is there.

### Parts 4–6, as delivered

All three landed in Sessions D and E. Part 4 became the Universe Inspector, Part 5
the dual view, Part 6 this file and the README.

**A note on this machine, kept because it will bite the next person.** Norton
holds freshly built binaries in a kernel-mode wait — one thread, zero CPU,
surviving `Kill()` — and it intercepts TLS, presenting its own root for every
host. Neither is a MarketScan problem and both look like one. The binary hold
needs a **SONAR / Auto-Protect** exclusion, which is a different setting from the
scan exclusion; with that in place a build that links and launches twenty
binaries at once for Catch2 discovery *still* stalls, so `-j 3`. The TLS
interception matters only when diagnosing a network failure, where it will make
any certificate look wrong.

## Known limitations, by name

Not bugs. Things this cannot do, written down so they are not rediscovered.

| | |
|---|---|
| **The assumed exit** | The biggest one, and now named on both sides rather than merely known. `DelistingReturnModel` selects between liquidating at the last price (`last_price`, the default) and applying a supplied delisting return (`apply_true_return`). On real data there is nothing to apply until CRSP delisting returns arrive, so every number over a universe with departures in it carries `last_price` — worth **29.42 points of return and 3.14 of Sharpe** on the churn parity fixture. See *the assumed exit* in [`EXECUTION.md`](EXECUTION.md). |
| **A fill is the next close, not a real next price** | Fixed as far as a daily bar allows. A cross-sectional decision on session N now fills at each symbol's session N+1 close, asserted by the lookahead invariant. A real order sent after a close fills *somewhere in* the next session, not at its close, and nothing here models where. See *the assumed instant* in [`EXECUTION.md`](EXECUTION.md). |
| **Renames are not inferred** | From ticker-keyed data a rename is indistinguishable from one name leaving and another arriving. `add_ticker` declares one; nothing populates it. |
| **No corporate actions** | No spin-offs, mergers, rights issues or share classes. |
| **No point-in-time restatements** | Prices are as they are today, adjustments included. |
| **Terminal order status** | A cancel, rejection or expiry produces no fill, so nothing tells the plugin the order is dead. Needs ABI v2; deliberately deferred. |
| **Free data is not research-grade** | Every number from it is a statement about this pipeline, not about markets. |
| **The research layer is frictionless** | No commission, no spread, no slippage — the engine owns execution, and a second cost model here would be a second answer to a question that already has one. Every Sharpe in `research/` reads better than anything tradeable. |
| **The null is Gaussian** | The false-discovery threshold was measured against a market factor plus normal noise. Real returns have fat tails, volatility clustering and cross-sectional structure that panel does not contain, all of which give a search more to find. The measured threshold is a floor. |
| **No research result is a market claim** | Every panel is synthetic and every λ was planted. Nothing in `research/` says anything about markets, and it is not built to. |
| **The IC t-statistic is anti-conservative** | Fires 8–26% of the time on data with no signal, worsening with the horizon. Not a defect to be fixed but a measured property, and the empirical bar in `research/thresholds.py` is what results are judged against. The t-stat is retained for screening, with its false-positive rate attached at the call site. |

---

## The two properties everything rests on

Worth re-reading before changing anything structural.

> **A strategy cannot tell whether it is being backtested, paper traded, or
> trading live.** One engine, one ledger, one `IBroker`. Only the clock and the
> transport swap. If you find yourself writing a second code path for "backtest
> mode", go back.

That was an assertion for eight sessions. As of Session F it is a measurement,
and it is worth being precise about how much of it the measurement covers.

**What is now evidence.** One plugin binary ran against the paper venue while
the market data was recorded, then ran again -- the same file, not a rebuild --
against that recording. The two produced 10 decisions that matched field by
field: symbol, side, quantity, type, time in force, limit price, the strategy's
own client order id, and the timestamp of the event that triggered each one.
Event counts matched too, 463 against 463. A divergence would have been
reported as the smallest differing decision rather than absorbed.

**What is still an assertion.** That a *fill-dependent* strategy would agree.
It would not, and should not: live it sees the venue's fills and in replay the
simulator's. The plugin used is deliberately written so its decisions are a
function of the event stream alone. So the measurement establishes that the
*pipeline* is identical, not that every strategy is indifferent to which side
of the seam it is on.

**What is structural rather than measured.** There is no mode flag to find.
`IBroker` has two implementations and `StrategyRuntimeConfig::broker` chooses
one; no `#ifdef`, no branch on "are we live" in the engine, in
`StrategyInstance`, or in any plugin. That is checkable by reading, and the
equivalence test is what would notice if it stopped being true.

**One asymmetry that is real and is not hidden.** `LiveBroker::submit` makes a
synchronous HTTPS request on the engine thread, so submitting an order stalls
event dispatch for a round trip -- about 60ms here. In replay the same call is
free. A strategy cannot *observe* which it got, but a busy one would feel it.

> **`theories/` is a USB port.** A compiled plugin dropped in is discovered at
> runtime, given its own virtual account, and appears in the GUI — with no host
> recompile and no host code that knows it exists.

---

## Session F: the vacuous-test audit and the live execution seam

| | | |
|---|---|---|
| 0 | Vacuous-test audit | done â€” see [`AUDIT.md`](AUDIT.md). One more shape of it was then found in Part 5 and is written up there too |
| 1 | Credentials and safety rails | done â€” paper-only enforced three ways, keys from the environment only |
| 2 | ABI v2 | done â€” one appended callback, a v1 fixture that trades and is asserted never to receive it |
| 3 | Live market data and the recorder | done â€” 3,215 real events in one 45-second session, 0 dropped, recorder byte-faithful |
| 4 | The broker | done â€” real fills, cancels, a real rejection, a reconnect that reclaimed 4 live orders |
| 5 | The equivalence test | done â€” 10 decisions identical live and in replay, and three earlier vacuous passes refused |
| 6 | Lifecycle exercise theory | done â€” `theories/lifecycle_exercise`, which loses money by design and says so |
| 7 | Documentation | this, plus the live section of [`EXECUTION.md`](EXECUTION.md) |

### What the live layer cost in real bugs

Worth listing, because every one of them was found by running the thing rather
than by reading it, and three of them were wrong guesses corrected by
measurement.

| what looked wrong | what was actually wrong |
|---|---|
| A feed reporting 60,236 ms of backwards jitter | Nothing. A one-minute bar is stamped at the start of its minute. Bars and ticks now have separate counters |
| Quote sizes needing a lot multiplier | They did not. A capture showed sizes of 40â€“1880 against trades of 1â€“200 shares. The multiplier was removed |
| Three TLS connections overwhelming the machine's inspection proxy | Nothing. `ms_live probe` measured 60ms per request with both sockets open. The guess was plausible and wrong |
| A POST hanging | It did not. 422 in 63ms |
| Everything hanging | WinHTTP's synchronous WebSocket receive honours **no timeout at all** â€” not the session's, not the request's, and `WINHTTP_OPTION_RECEIVE_TIMEOUT` on the socket is accepted and ignored. A silent socket's pump thread could not be joined |
| A replay disagreeing with its own live session by 0.2 ms of mean skew | Two definitions of one number: the replay measured skew *after* the monotonic gate raised timestamps. There is now one definition, in one place, with a test holding it there |
| A reconciliation that would not go green | It was right. Three shares of QQQ on the account belonged to a run that had died mid-session. Traced order by order |
| A run crashing at the reclaim | `"filled_avg_price": null` on an unfilled order, and `json::value` throws on present-but-null with a string default |

### What is owed to the next phase

- **A regular-session run.** Every live number here was measured after 16:00 ET,
  where IEX carried one of the five symbols asked for. Regular hours would be a
  stronger measurement of the same things.
- **A partial fill against a venue.** Not reachable with one-share orders.
- **The equivalence test cannot be in CI**, and a result nobody can re-run on
  demand is weaker than one that runs nightly. It is quoted with its date
  instead.
- **Nothing here is a statement about execution quality.** Paper fills are the
  venue's simulation, IEX is two per cent of volume, and
  [`DATA_POLICY.md`](DATA_POLICY.md) Â§7 says both on its face.
