# Status

Where the project is, what to do next, and what is blocked.

**Last updated:** 2026-09-18, end of Session A (`6fe8f57`). CI green.

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
```

Nothing else is machine-specific. The tests do not touch the network and the
committed fixtures are synthetic.

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

Milestones M0–M9 of Spec 1, and M0–M3b of Spec 2, are done. Specs are in
[`specs/`](specs/).

**327 tests** on `debug`, `release`, `ci`, `ci-debug`; **328** on `full`.

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

**Session B — Spec 2 M4–M8, the research harness.** Python, `research/`, and
a fresh session: the spec says so and it is right, because none of it touches
the C++ engine.

1. **M4** — synthetic panel generator with a known embedded signal λ.
2. **M5** — the research pipeline: point-in-time features, purged and embargoed
   k-fold, ridge and GBM, portfolio construction with active weights.
3. **M6** — ground truth recovery. Does the pipeline find the λ that was
   planted?
4. **M7** — pathology injection: survivorship, lookahead, delisting. Assert each
   guard fires, and record the magnitude.
5. **M8** — false discovery rate at λ = 0. **This is the one that matters.** Run
   200+ pipeline variants over data containing no signal and count how many
   claim one.

Read `specs/BUILD_SPEC_2.md` §7 before starting. None of it needs the Stooq
data — the whole point of M4–M8 is synthetic ground truth.

**Then Session C — M9** (parity test, Universe Inspector panel) and **M9b**
(`specs/SPEC_VIEW_MODES.md`, dual GUI presentation modes, not started).

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
