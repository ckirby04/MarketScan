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

### Still open, and it is Session D Part 3

The sizing-side lookahead is gone. The **fill-side** one is not even measured
yet. Both layers currently fill a decision taken from session N's close *at*
session N's close, which Session D argues is itself a lookahead — trading on
information simultaneously with acting on it. Resolving it means instrumenting
the fill path with a decision/price/fill timestamp triple and asserting that a
fill price is stamped later than the bar that produced the decision. Expect it to
move committed results.

---

## Session D: where it stands

| | | |
|---|---|---|
| 0 | Housekeeping, reboot, baseline | **not done** — see below |
| 1 | Bit-identical sizing | **done**, CI green |
| 2 | Make delisting returns real | not started |
| 3 | The lookahead probe | not started; the sizing half of it landed in Part 1 |
| 4 | Toolchain gate (GUI stack) | not reached |
| 5–7 | Inspector, dual view, close-out | not started |

**Part 0 was skipped and should not have been, except that its substance did not
block Part 1.** It asks for a reboot first: this machine's endpoint security
holds freshly built binaries in a kernel-mode wait — one thread, zero CPU,
surviving `Kill()` — and stale copies keep their own executables open, so
`test_parity.exe` cannot relink. 26 such processes are outstanding. Part 1 was
finished by compiling locally and verifying through CI, which worked but cost
several seven-minute round-trips for answers a local run gives in seconds. Parts
2 and 3 involve real iteration and should not be attempted that way.

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
| 3 | M9 parity | churn-free parity and the cost check done; **churn parity not run** |
| 4 | Universe Inspector panel | not started |
| 5 | M9b, the dual view | not started |
| 6 | Close-out: README and this file | partly — this file is current, `README.md` is not |

### What Part 3 still owes

**Churn parity.** Only the churn-free case has been run, which is the ordering
Session C asks for and the half that had to pass first. The churn case needs a
second fixture with departures, a `Universe` built in the test so the engine's
forced-exit path fires, and the gap quantified and attributed to names.

Expect it to be *smaller* than it sounds, and be careful about why. The engine
force-liquidates at the last price the book has, and in a synthetic panel the
departure return **is** the final bar — so the engine captures it rather than
missing it, and the two layers may agree for a reason that would not hold on real
data where the drop is not in the file. That is worth confirming explicitly
rather than reporting as a clean pass: what the churn case actually measures here
is the *timing* of the exit, not the size of the assumed one.

**The one-symbol lookahead, predicted and not observed.** Reading the engine says
an order submitted inside a callback fills immediately against the book as of that
event, and `on_session_close` fires while the first bar of the *next* session is
being dispatched — so one symbol per session should price at tomorrow's close.
Four rebalances matched exactly, so either the reading is wrong or the effect is
masked by the fixture. It wants a purpose-built probe: submit for every symbol at
a session close, record the fill prices, compare each to that session's close and
the next one's. An inference from a curve that happened to agree is not an answer.

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
| **Forced exits are optimistic** | The biggest one. A name leaving the index is liquidated at the last price; real delisting returns are frequently −30% or everything. This is where naive backtests hide their losses. |
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
