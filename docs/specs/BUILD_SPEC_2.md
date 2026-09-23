# MarketScan — Build Specification 2: Data Layer, Universe, and Research Harness

**Repo:** `https://github.com/ckirby04/MarketScan` (chassis complete through M9 of Spec 1)
**Prerequisite reading:** `BUILD_SPEC.md` — Section 2 (the NOT-list) and Section 4 (the plugin ABI)
still bind. Nothing in this document relaxes them.

---

## 0. How to use this document

Spec 1 built the chassis. This spec builds the **fuel system and the dynamometer**: a daily-bar
data layer with point-in-time universe handling, and a research harness that can prove your
pipeline works before any real market data exists.

**Run this across three sessions, not one.** Spec 1 ran seven hours in a single context and CI
silently died in the middle without anyone noticing for two hours. Split it:

| Session | Milestones | Nature |
|---|---|---|
| A | M0 – M3 | C++ — audit, session boundaries, data layer, universe |
| B | M4 – M8 | Python — synthetic panel, research pipeline, validation harness |
| C | M9 | Integration, parity test, docs |

Start each session by re-reading `BUILD_SPEC.md` Sections 2 and 4, then this document. Re-read
this document before each milestone. It is on disk; re-grounding is nearly free and it is the
only defense against compaction drift.

Milestone order is fixed. Definition of Done gates each one. Section 8 lists STOP AND ASK
triggers — the Spec 1 triggers remain in force and this document adds to them.

---

## 1. What this phase is for

Two things, and they are deliberately separate:

**The data layer (C++, M1–M3)** teaches the engine about daily bars, about a *universe* of
symbols that changes over time, and about the fact that a company which was not in the index on
a given date must not be visible to a strategy on that date. That last property is the whole
reason this milestone exists, and it must be **structural** — enforced by the engine, not by a
strategy author remembering to check.

**The research harness (Python, M4–M8)** is a dynamometer. It generates synthetic market panels
with a *known* embedded signal, runs the full research pipeline against them, and verifies the
pipeline recovers what was planted. Then it injects deliberate pathologies — survivorship bias,
lookahead leakage, delistings — and verifies the guards fire. Then it plants *nothing at all*
and measures how often the pipeline claims to find something anyway.

That last number is the point. Every result this project ever produces is worth exactly as much
as the pipeline's false-discovery rate, and with synthetic data you can measure it instead of
arguing about it.

**Critical architectural point:** factor research does **not** run through the C++ event engine.
It runs in pandas. The engine executes a strategy once you have one; the research that finds one
is a cross-sectional panel problem and forcing it through an event loop would be miserable and
slow. The two meet at M9, where the same strategy is implemented both ways and the results must
agree.

---

## 2. What I do NOT want

Spec 1 Section 2 still applies in full. Additions:

**Architecture**

- Do **NOT** modify `include/marketscan/abi/plugin_abi.h`. The ABI is frozen at v1. Session
  boundary detection goes in the **C++ wrapper layer**, not the C boundary. If you believe a
  requirement here cannot be met without an ABI change, that is a STOP AND ASK — do not
  make the change.
- Do **NOT** implement the research pipeline in C++ or route it through the event engine.
  M4–M8 are Python, pandas, numpy, scipy, scikit-learn.
- Do **NOT** enforce universe membership by convention, documentation, or a helper a strategy
  is expected to call. A strategy must be **structurally incapable** of seeing a symbol that
  was not a member on the current date.

**Data**

- Do **NOT** commit market data. The cache directory is gitignored. Tests use small committed
  synthetic fixtures, never network fetches.
- Do **NOT** fetch anything from the network inside a test. Ever. Tests must run offline in CI.
- Do **NOT** hammer free data sources. Fetch politely, cache aggressively, never re-fetch what
  is already on disk, and make the fetcher resumable.
- Do **NOT** treat the free data stack as research-grade. It exists to exercise machinery.
  Anywhere a number derived from it could be mistaken for a result, label it.

**Research**

- Do **NOT** implement momentum, value, quality, or any other real financial factor. This phase
  validates the pipeline against synthetic ground truth. Real factors wait for real data.
- Do **NOT** use standard k-fold cross-validation on panel data. Purged and embargoed only.
- Do **NOT** report a backtest result from this phase as though it means anything about markets.
  Every number in this phase is a statement about the *pipeline*, not about the world.

---

## 3. M0 — Chassis audit

Before building on the chassis, verify it is what Spec 1 asked for. Report findings plainly,
including any that are unflattering. Fix what is broken.

**Compliance checks against Spec 1 Section 2** — report each as pass/fail with evidence:

1. `plugin_abi.h` contains no C++ types. Grep for `std::`, `class`, `template`, `throw`.
   Expect zero hits.
2. Cash and realized P&L use `int64_t` micro-dollars, not `double`, everywhere in the ledger.
3. The engine's **default** fill model is `SpreadCrossingFillModel`. Trace the actual default
   construction path, not the comment above it.
4. No hash-container iteration in any path that affects an emitted order.
5. No second code path for backtest vs. live — the divergence, if any, is confined to the clock
   and the transport.
6. No allocation, lock, or syscall in the per-event hot path.

**Known loose ends to close:**

7. The per-strategy RNG (`random_uniform`) is unexercised — neither reference theory calls it.
   Write a test theory that draws from it and prove replay determinism holds across runs and
   that two strategies drawing concurrently do not perturb each other's sequences.
8. `MARKETSCAN_WERROR` was off through M0–M8 of Spec 1. Enable it locally, fix the accumulated
   warnings, report the count.
9. Resolve why the `full` preset runs 215 tests and the others run 214. Document it in the
   preset comments.
10. Audit Spec 1's milestone commit messages against what the tests actually assert. One was
    already found claiming a test that did not exist. Report any others.

**Definition of Done:** an `docs/AUDIT.md` recording every check and its result; all failures
either fixed or escalated; `WERROR` clean; tests green.

---

## 4. M1 — Session boundaries and cross-sectional strategies (C++)

Cross-sectional strategies need to see every symbol at once, at a defined point in time. The
per-symbol `on_bar` dispatch cannot express that. Solve it in the C++ wrapper.

- The wrapper tracks the session date across incoming events. When the date rolls, it invokes a
  new virtual `on_session_close(MS_Nanos session_end)` **before** delivering the first event of
  the new session.
- Add a `CrossSectionalStrategy` convenience base deriving from `IStrategy` that buffers the
  session's bars into a symbol-indexed snapshot and hands it to the subclass as a single
  structure at session close. Subclasses override one method and receive the full cross-section.
- Both mechanisms are wrapper-side. `plugin_abi.h` does not change. Existing compiled plugins
  must continue to load and behave identically.

**Session boundary definition:** a session rolls when the event timestamp crosses midnight in a
configurable exchange timezone (default `America/New_York`). Do not use UTC midnight. Do not
infer it from gaps in the data.

**Definition of Done:** `on_session_close` fires exactly once per session with correct
boundaries across DST transitions; a test cross-sectional theory receives the full snapshot;
`buy_and_hold` and `sma_crossover` DLLs built against Spec 1 still load and produce byte-identical
results.

---

## 5. M2 — Daily bars and the free data stack

- Extend the data layer to daily bars alongside the existing intraday path. One `IMarketDataFeed`
  implementation, one event type, configurable frequency.
- A **Stooq** fetcher (Python, under `scripts/`): free daily OHLCV, no API key. Fetch politely,
  write to a local cache under `data/cache/` (gitignored), never re-fetch a symbol-range already
  cached, resumable after interruption, and honest about failures — a missing symbol is logged,
  not silently skipped.
- A cache format the C++ side reads directly. CSV is acceptable; a simple binary format is better
  and faster to replay. Your call, document it.
- Commit a **small** synthetic fixture set for tests. Real data never enters the repo.

**Definition of Done:** `marketscan.exe` replays a multi-year daily dataset for a few hundred
symbols; fetch-and-cache is idempotent and resumable; every test runs offline.

> **Editorial note added in Session G, 2026-09-23.** The Stooq requirement above is
> **superseded by observation and was never met.** Stooq put a proof-of-work browser
> challenge in front of that endpoint; solving it programmatically was considered and
> declined. `scripts/fetch_stooq.py` exists and is kept because it reports the gate
> correctly rather than because it fetches anything. See
> [`docs/DATA_POLICY.md`](../DATA_POLICY.md) for the decision and
> [`docs/STATUS.md`](../STATUS.md) for what M3b depends on instead. The original text is
> left exactly as written: this spec is a record of what was asked, not of what turned
> out to be possible.

---

## 6. M3 — Universe and point-in-time membership

The load-bearing milestone. Get this wrong and every result downstream is fiction.

- A `Universe` abstraction answering `members_on(date) -> set<SymbolId>`.
- A loader for the free membership history at `https://github.com/fja05680/sp500`, which
  reconstructs S&P 500 constituents back to 1996 with add/drop dates. Structure the loader
  behind an interface so a CRSP/Compustat source drops in later without touching callers.
- **The survivorship guard, structural:** when a universe is active, the engine delivers a bar
  for symbol S on date D to a strategy only if `S ∈ members_on(D)`. The host context's
  `symbol_count()` and symbol iteration reflect only current members. A strategy has no API by
  which it can observe a non-member. This is enforced in the engine's dispatch, not documented
  as a rule.
- **Delisting / removal handling.** When a held symbol leaves the universe, the engine
  force-liquidates the position at the last available price on the final membership date, logs
  it distinctly in the blotter as a forced exit, and credits proceeds to cash. Note in the code
  and the README that this is an approximation: with CRSP delisting returns it will be replaced
  by the actual delisting return, which is frequently far worse and is exactly where naive
  backtests hide their losses.
- A test that proves the guard works: construct a universe where symbol X is a member only in
  2015, replay 2013–2017, and assert the strategy observed X on no date outside 2015.

**Definition of Done:** universe loads from the free source; the guard test passes; forced-exit
path tested; a `--no-universe` mode exists for intraday work and is clearly distinct.

---

## 7. M4 – M8 — The research harness (Python)

All of this lives under `research/`, is pure Python, and does not touch the C++ engine.

### M4 — Synthetic panel generator

Generate a panel of N synthetic securities over T periods with returns constructed as:

```
r_it = β_i · m_t  +  λ · x_it  +  ε_it
```

where `m_t` is a market factor, `x_it` is a per-name exposure you generate and record, `ε_it` is
idiosyncratic noise, and **λ is a parameter you set**. Everything — N, T, λ, the β distribution,
noise scale, exposure autocorrelation — is configurable and seeded. The generator also emits a
synthetic membership history so the universe machinery is exercised.

**Definition of Done:** generator is deterministic given a seed; the realized cross-sectional
relationship between `x` and forward returns matches the configured λ to within sampling error,
verified by a test.

### M5 — Research pipeline

A feature/label pipeline over a panel: point-in-time-safe feature construction, cross-sectional
ranking and normalization per date, forward-return labels with configurable horizon, **purged
and embargoed k-fold** cross-validation (drop training samples whose label windows overlap the
test fold, then apply a gap), a ridge and a gradient-boosted model, and a portfolio construction
layer producing **active weights** against the benchmark with sector-neutrality, per-name caps,
beta targeting, and a turnover budget.

The features here are generic panel transforms. Do **not** implement momentum, value, or any
named financial factor — the synthetic exposure `x_it` is the only signal this phase consumes.

**Definition of Done:** pipeline runs end to end on a generated panel; purging verified by a test
that asserts zero overlap between train and test label windows; portfolio constraints verified
by tests that violate each one and confirm rejection.

### M6 — Ground truth recovery

Run the full pipeline against a panel with known λ. Assert it recovers λ within error bars across
a sweep of λ values including small ones. Produce a recovery plot: estimated vs. true λ.

**Definition of Done:** recovery test passes across at least five λ values; the plot is committed.

### M7 — Pathology injection

For each, inject deliberately and assert the expected consequence:

1. **Survivorship** — build the universe from final-period survivors only. Assert the measured
   premium inflates materially versus the point-in-time universe. Record the magnitude; that
   number is how much survivorship bias is worth in your own numbers.
2. **Lookahead** — leak a `t+1` return into a feature. Assert the resulting out-of-sample R²
   exceeds a threshold no honest financial feature reaches. Record what the smell looks like.
3. **Delisting** — force a name to -100% and exit the universe. Assert the ledger and the
   pipeline both handle it without silently dropping the loss.

**Definition of Done:** three tests, each asserting a quantitative consequence, each with the
observed magnitude written into `docs/PATHOLOGIES.md`.

### M8 — False discovery rate

Set **λ = 0**. No signal exists in the data at all. Run a large sweep of pipeline variants —
different features, horizons, models, hyperparameters; at least 200 — and count how many report a
"significant" result at conventional thresholds.

Report the empirical false discovery rate, the distribution of the best-of-N Sharpe under the
null, and the Sharpe threshold that a strategy must clear to be distinguishable from noise given
the number of variants tested.

**Definition of Done:** the sweep runs reproducibly from one command; results and the derived
threshold are committed to `docs/FALSE_DISCOVERY.md`; the threshold is exposed as a constant the
research code can import, so future results are judged against a measured bar rather than a
remembered one.

---

## 8. M9 — Parity and integration

Implement one trivial strategy — long the top decile of the synthetic exposure, short the
bottom, monthly rebalance — **twice**: once in the Python research pipeline and once as a C++
theory plugin using `CrossSectionalStrategy`. Run both over the same synthetic panel.

Under optimistic fills and zero costs the equity curves must match to within floating-point
tolerance. Under spread-crossing fills the C++ result should be worse by an amount consistent
with the modeled costs. Assert both.

If they disagree beyond tolerance, one of them is wrong and finding out now is the entire point
of the milestone.

Also in M9: a Universe Inspector panel in the GUI (membership over time, current members,
add/drop events, forced exits), and documentation updates.

**Definition of Done:** parity test in CI; inspector panel working; `README.md` updated to
describe the research layer and to state plainly that the free data stack is for machinery, not
conclusions.

---

## 9. STOP AND ASK triggers

Spec 1's eight triggers remain in force. Additionally:

1. Any requirement here that appears to need a `plugin_abi.h` change.
2. Any case where the structural survivorship guard would have to become a convention or a
   documented rule instead of an enforced one.
3. Financial semantics not specified here: index rebalance mechanics, spin-offs, mergers,
   ticker changes and reuse, multiple share classes, halts.
4. If the false-discovery sweep at M8 produces a threshold that would invalidate the approach —
   say so, loudly, rather than shipping it quietly in a docs file.
5. If a milestone's Definition of Done cannot be met without weakening the assertion it makes.

---

## 10. What "done" looks like

The engine replays two decades of daily bars across a universe whose membership changes
correctly over time, and a strategy cannot see a company that was not in the index that day
because the API gives it no way to. Alongside it, a research pipeline that has been proven to
recover a signal that was planted, proven to be fooled by biases you injected on purpose, and
measured for how often it finds signal in data containing none.

No real market data is in the repo. No real factor is implemented. Nothing here claims anything
about markets.

What it claims is that the instrument is calibrated — and that is the only thing worth knowing
before the real data arrives.
