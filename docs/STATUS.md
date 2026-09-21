# Status

Where the project is, what to do next, and what is blocked.

**Last updated:** 2026-09-18, end of Session B. CI green.

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
./scripts/dev-env.ps1 ctest --preset release          # 327 tests

# 3. The index membership history. This one still works.
python scripts/fetch_sp500.py
marketscan.exe --headless --build-universe data/cache/universe/sp500_history.csv

# 4. Price data. This one does NOT work -- see the blocker below.
python scripts/fetch_stooq.py --symbols AAPL

# 5. The research layer. Pure Python, no vcpkg, no MSVC, no engine.
pip install -r research/requirements.txt
pytest
```

Nothing else is machine-specific. The tests do not touch the network and the
committed fixtures are synthetic.

The research layer is worth knowing about separately: it needs none of the C++
toolchain. On a machine with Python and no vcpkg -- which is a normal state for
a machine that has only ever run the analysis -- `pytest` and
`python -m research.sweep` both work from a fresh clone.

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
| **GUI** | twelve panels, ImGui + ImPlot | `docs/screenshots/dashboard.png` |
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

One micro-dollar, on one session, in `data/samples/parity_expected.csv`. Caught
by the equality assertion itself.

### Churn parity, added in Session D Part 2

A second fixture, `data/samples/parity_churn_*`, where names leave: 29
departures, a membership history the engine builds a `Universe` from, and a
delisting return table the generator knows the truth of. Both layers are asserted
identical **under each setting of `DelistingReturnModel`**, which is what splits
agreement about the *timing* of a forced exit from agreement about its
*magnitude* — the distinction the old fixture could not make, because the
departure return used to be the departing name's final bar. See *What Part 3
owed* below.

### Still open, and it is Session D Part 3

The sizing-side lookahead is gone. The **fill-side** one is now measured — churn
parity caught it, at $606.07 on one symbol at session 1 of 126 — but not fixed.
Both layers fill a decision taken from session N's close *at* session N's close
for every symbol but one, which is trading on information simultaneously with
acting on it; the one exception fills a session late, which is the only fill in
the run that is actually defensible. Resolving it means the decision/price/fill
timestamp triple, the invariant asserted in CI, and a change to
`CrossSectionalStrategy` so that every fill lands a session after the decision.
Expect it to move committed results. Detail in *What Part 3 owed, and what it
got*.

---

## Session D: where it stands

| | | |
|---|---|---|
| 0 | Housekeeping, reboot, baseline | **done** — baseline below |
| 1 | Bit-identical sizing | **done**, CI green |
| 2 | Make delisting returns real | **done** — both layers, churn parity exact |
| 3 | The lookahead probe | not started; the sizing half landed in Part 1, and Part 2 **measured** the fill-timing half |
| 4 | Toolchain gate (GUI stack) | not reached |
| 5–7 | Inspector, dual view, close-out | not started |

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

The one skipped test is the identity classifier's calibration case, which needs
cached Stooq bars; see *Stooq is gated* below.

Part 2 then moved them, and the movement is accounted for: **343 C++** (+9 cases,
all in `test_parity.cpp` — the churn fixture, the two curve comparisons, the
row-for-row reconciliation, the flattery gate, the rolling-symbol property and the
two loader cases) and **199 Python** (+14, all in `research/tests/test_parity.py`).
`clang-format` 19.1.1 clean across the tree. No existing test changed its result.

---

## Blocked — needs a decision

### 1. Stooq is gated, so identity cannot be measured

As of 2026-09-17 Stooq answers every request to its free CSV endpoint with
**HTTP 200 and a JavaScript browser-verification page**. Nothing here attempts
to defeat that check.

The consequence is narrow but real: the M3b identity classifier is built,
tested and calibrated, and has **nothing to point at**. All 53 membership gaps
read `unresolved: no price data`, so the 730-day threshold still governs and all
**41 splits remain inferences**.

Everything else works without it. The cache format, the feed, the universe, the
guard and the classifier are all exercised against committed synthetic fixtures.

**The options, none of which should be picked without deciding:**

| | |
|---|---|
| Wait | The gate may be temporary. Costs nothing, resolves nothing. |
| Another free source | Every one comes with a key, an account, or terms nobody here has read. Spec 1 forbids keys and accounts; Spec 2 named Stooq specifically. **This is a spec change, not a fix.** |
| A licensed source | CRSP deletes this problem entirely — PERMNO *is* the identity, and the whole inference heuristic gets deleted rather than adapted. |
| Ship without it | Legitimate. The splits stay inferences, the report says so, and the safe direction protects you. |

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

## Next

**Session C is part-done.** Parts 0–3 of `MARKETSCAN_SESSION_C.md` are complete
except where noted; Parts 4–6 have not been started, which is the boundary that
document itself nominates as clean.

| | | |
|---|---|---|
| 0 | Commit Session B | done — five milestone commits, CI green |
| 1 | M8b, empirical IC thresholds | done |
| 2 | What moves the bar | done |
| 3 | M9 parity | done — churn-free parity, the cost check, and (in Session D Part 2) **churn parity, exact under both delisting models** |
| 4 | Universe Inspector panel | not started |
| 5 | M9b, the dual view | not started |
| 6 | Close-out: README and this file | partly — this file is current, `README.md` is not |

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

What the assumption is worth on that fixture: **30.10 points of return and 3.55 of
Sharpe** (+11.74% and +2.41 assuming a zero delisting return, −18.36% and −1.14
applying it). +2.41 clears the 1.927 null-Sharpe threshold that
`FALSE_DISCOVERY.md` derived from a 1,512-run sweep, so under the optimistic
assumption this rule looks distinguishable from noise after adjusting for the size
of the search, and under the honest one it loses money. Full detail in **the
assumed exit** in `EXECUTION.md`.

**The one-symbol lookahead: observed, and measured.** This file previously said it
was *predicted and not observed*, and that an inference from a curve that happened
to agree is not an answer. It is observed now, and it was churn parity that caught
it rather than a probe built to look for it.

The reading was correct. `Engine::step` applies an event to the book before
dispatching it, so that a strategy and the fill model pricing its order see the
same market — right for a strategy reacting to a print. But
`IStrategy::advance_session` fires `on_session_close(N)` from inside the dispatch
of the **first bar of session N+1**, so at the instant a session-N decision is
priced, exactly one symbol's book is already a session ahead. An order in that
symbol is sized against session N's close and filled at N+1's.

Measured, on the churn fixture before it carried a dedicated roll symbol: the
short leg held SY0000, which sorts first on every session. It was sized against a
close of 148.6207 and filled at 143.216 — **$606.07 apart at session 1 of 126,
growing to $942 by the end**. Caught by
`tests/test_parity.cpp`'s "under last_price the two layers agree about timing,
exactly", which is not a test anybody wrote to look for a lookahead.

Why the churn-free fixture never showed it: SY0000 sorts first there too and
**never once lands in either decile**. That is luck, not a property anybody chose,
and the parity suite depended on it silently. It no longer does —
`the symbol that rolls each session is never in a traded leg` asserts it on both
fixtures, and the churn fixture satisfies it deliberately with a `ROLL` symbol
whose exposure is pinned mid-rank.

**So Part 3's remaining work is the fix, not the diagnosis.** The timestamp triple
and the invariant are still owed: record, per fill, the decision's information
horizon, the timestamp of the price used, and the fill's own, then assert that a
decision taken from bar N can only fill against a price timestamped later than bar
N's close. Note what that invariant says about the engine as it stands — the one
anomalous symbol is the only one that *satisfies* it. The other 29 fill at the
close they were sized from, which is the larger lookahead and the one that will
move committed results when it is fixed.

The fix cannot be "fire the session-close callback earlier": `plugin_abi.h` is
frozen at v1 and its vtable has no session hook, so the shim can only learn a
session ended by being handed the next one's first bar. It belongs in
`CrossSectionalStrategy` — compute targets on session N's cross-section, submit
each symbol's order when that symbol's next bar arrives — which needs no ABI
change and makes every fill a session later than the decision, uniformly.
### Parts 4–6

Read `specs/SPEC_VIEW_MODES.md` in full before starting Part 5; it is binding as
written. Both need the GUI stack, so the first step is `cmake --preset release`,
which builds ImGui, ImPlot and GLFW from source and takes a while on a cold
vcpkg cache. Everything in Parts 0–3 was done on the `ci` preset, which needs
none of that.

**A note on this machine.** Its endpoint security holds freshly built binaries in
a kernel-mode wait — they start with one thread, zero CPU, and survive `Kill()`.
An exclusion for the repository fixed it enough to run one binary at a time; a
build that launches twenty at once for Catch2 test discovery still stalls, and
stuck copies hold their own executables open and block the next link. CI builds
and runs everything cleanly, so it is the verifier of last resort here. A reboot
clears the stuck processes.

---

## Known limitations, by name

Not bugs. Things this cannot do, written down so they are not rediscovered.

| | |
|---|---|
| **The assumed exit** | The biggest one, and now named on both sides rather than merely known. `DelistingReturnModel` selects between liquidating at the last price (`last_price`, the default) and applying a supplied delisting return (`apply_true_return`). On real data there is nothing to apply until CRSP delisting returns arrive, so every number over a universe with departures in it carries `last_price` — worth **30.10 points of return and 3.55 of Sharpe** on the churn parity fixture. See *the assumed exit* in [`EXECUTION.md`](EXECUTION.md). |
| **One fill per session is priced a session early, and the rest a session late** | Measured, not fixed. A cross-sectional decision on session N fills at session N's own close for every symbol but the one whose bar rolled the session, which fills at N+1's. Session D Part 3. |
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
> trading live.** One engine, one ledger, one fill simulator. Only the clock and
> the transport swap. If you find yourself writing a second code path for
> "backtest mode", go back.

> **`theories/` is a USB port.** A compiled plugin dropped in is discovered at
> runtime, given its own virtual account, and appears in the GUI — with no host
> recompile and no host code that knows it exists.
