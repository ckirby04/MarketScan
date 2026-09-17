# MarketScan — Autonomous Build Specification

**Target repo:** `https://github.com/ckirby04/MarketScan` (currently empty)
**Local working dir:** the empty `Quant` folder on this machine
**Platform:** Windows 11, MSVC (Visual Studio 2022 toolchain), C++20
**Build system:** CMake ≥ 3.25 + CMakePresets + vcpkg (manifest mode)

---

## 0. How to use this document

You are building the **chassis** of a quantitative trading research platform. You are **not**
building trading strategies. Read this entire document before writing code.

Work through the milestones in order. Each milestone has an explicit **Definition of Done**.
Do not start milestone N+1 until milestone N builds, passes its tests, and is committed.

When you hit a **STOP AND ASK** trigger (Section 9), stop, write your question clearly, and wait.
Do not guess on those. Everywhere else, use your judgment and keep moving.

---

## 1. What this project is

MarketScan is a C++ desktop application that runs trading strategies against market data —
historical replay, simulated live, or (eventually) real live execution — and shows the results
in a rich GUI. The defining architectural property:

> **A strategy cannot tell whether it is being backtested, paper traded, or trading live.**

The same strategy binary, the same engine code, the same ledger, the same fill simulator.
Only the *clock* and the *transport* swap out. Every design decision in this document serves
that property. If you find yourself writing a second code path for "backtest mode," you have
made a mistake — go back.

The second defining property is the **theories/ folder as a USB port**: a compiled strategy
plugin dropped into `theories/` is discovered at runtime, loaded, given its own virtual
account, and appears in the GUI — with no recompile of the host application and no changes to
host code.

---

## 2. Non-negotiable constraints — what I do NOT want

These are hard rules. Violating one is a bug even if everything compiles and passes tests.

**Architecture**

- Do **NOT** write separate code paths for backtest vs. live. One engine, one strategy
  interface, swappable clock and transport.
- Do **NOT** let strategy code call `std::chrono::system_clock`, `time()`, `rand()`,
  unseeded RNG, file I/O, or network I/O. Time comes from the host context. Randomness comes
  from a host-provided seeded generator. This is what makes replay deterministic.
- Do **NOT** pass C++ types across the plugin boundary. No `std::string`, no `std::vector`,
  no exceptions, no `std::shared_ptr`. The boundary is **pure C**. On MSVC, debug and release
  CRTs have incompatible STL layouts and a plugin built in the wrong config will corrupt memory
  in ways that take days to find. The C ABI in Section 4 is not negotiable.
- Do **NOT** use `double` for cash or realized P&L accounting. Use `int64_t` micro-dollars
  (1 USD = 1'000'000). Prices may be `double`. Cash must be exact.
- Do **NOT** iterate `std::unordered_map` (or any hash container) anywhere a decision or an
  emitted order depends on the iteration order. Hash iteration order is not stable across runs.
  Use ordered containers or explicit stable sorts in those paths.
- Do **NOT** allocate, lock a mutex, or make a syscall in the per-event hot path. Preallocate
  and pool.

**Scope**

- Do **NOT** implement any real trading strategy. The only strategies in this build are the two
  reference stubs described in Section 6. No momentum, no regression, no factor models.
- Do **NOT** write any data scrapers, API clients, or integrations: no X/Twitter, no
  congressional disclosure, no news, no sentiment, no Alpaca, no Databento, no IBKR.
- Do **NOT** connect to anything over the network. v1 runs entirely offline.
- Do **NOT** add anything that requires an API key, a paid subscription, or an account.

**Implementation**

- Do **NOT** default the fill simulator to optimistic fills. The default must cross the spread.
  An optimistic model exists only as an explicitly-selected comparison baseline.
- Do **NOT** use Backtrader, Zipline, VectorBT, NautilusTrader, or any other existing backtest
  framework as the engine. We are building the engine.
- Do **NOT** build a web frontend, Electron, or any browser-based UI. Native ImGui only.
- Do **NOT** use Qt.
- Do **NOT** commit market data files, API keys, `.env` files, or anything in `build/`.
- Do **NOT** create placeholder files that don't compile, or stub functions that silently
  return zero. If something isn't implemented, it should not exist yet.
- Do **NOT** skip or disable a test to hit a milestone. If a test can't pass, that's a
  STOP AND ASK.
- Do **NOT** generate thousands of lines before building. Build frequently.

---

## 3. Repository layout

Initialize the repo in the `Quant` folder, wire up the existing GitHub remote, and create this
structure. Use `main` as the default branch.

```
MarketScan/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── .gitignore
├── .clang-format
├── README.md
├── LICENSE                          # MIT
├── .github/workflows/ci.yml
│
├── include/marketscan/
│   ├── abi/
│   │   ├── plugin_abi.h             # PURE C. The frozen boundary. See Section 4.
│   │   └── export.h                 # MS_EXPORT / dllimport-dllexport macros
│   ├── core/
│   │   ├── types.hpp                # Timestamp, Price, Quantity, SymbolId, Money
│   │   ├── money.hpp                # int64 micro-dollar fixed-point type
│   │   ├── clock.hpp                # IClock, SimulatedClock, SystemClock
│   │   ├── event.hpp                # MarketEvent variant
│   │   ├── ring_buffer.hpp          # lock-free SPSC
│   │   └── engine.hpp               # the event loop
│   ├── data/
│   │   ├── feed.hpp                 # IMarketDataFeed
│   │   ├── synthetic_feed.hpp       # seeded GBM tick/bar generator
│   │   └── csv_feed.hpp
│   ├── book/
│   │   └── order_book.hpp           # L2 book; MBO reconstruction comes later
│   ├── exec/
│   │   ├── order.hpp
│   │   ├── fill_model.hpp           # IFillModel + implementations
│   │   └── ledger.hpp               # per-strategy virtual account
│   ├── strategy/
│   │   ├── istrategy.hpp            # C++ ergonomic base class + export macro
│   │   ├── registry.hpp             # static built-in registration
│   │   ├── plugin_loader.hpp        # runtime DLL discovery/load/unload
│   │   └── host_context.hpp         # engine-side impl of MS_HostContext
│   ├── metrics/
│   │   ├── performance.hpp          # Sharpe, Sortino, DD, Calmar, alpha, beta, IR
│   │   ├── rolling.hpp              # rolling-window variants
│   │   └── correlation.hpp
│   └── util/
│       ├── latency_histogram.hpp    # HdrHistogram-style
│       └── logging.hpp
│
├── src/                             # mirrors include/ layout
│
├── apps/
│   └── marketscan/                  # the executable
│       ├── main.cpp
│       ├── app.cpp/.hpp             # app shell, docking, layout persistence
│       ├── theme.cpp/.hpp
│       └── panels/                  # one file per panel, see Section 7
│
├── theories/                        # THE USB PORT
│   ├── README.md                    # how to write and build a theory
│   ├── _template/                   # copy-this-to-start skeleton + its own CMakeLists
│   ├── buy_and_hold/                # reference: benchmark strategy
│   └── sma_crossover/               # reference: deliberately naive, for calibration
│
├── bindings/python/                 # pybind11
│   ├── CMakeLists.txt
│   ├── module.cpp
│   └── examples/quickstart.ipynb
│
├── tests/
│   ├── test_money.cpp
│   ├── test_ledger.cpp
│   ├── test_fill_model.cpp
│   ├── test_determinism.cpp
│   ├── test_plugin_loader.cpp
│   ├── test_metrics.cpp
│   └── test_ring_buffer.cpp
│
└── data/samples/
    └── synthetic_spy_1min.csv       # small, committed, for tests and first run
```

**Dependencies** (all via vcpkg manifest): `imgui[docking-experimental,glfw-binding,opengl3-binding]`,
`implot`, `glfw3`, `catch2`, `pybind11`, `spdlog`, `nlohmann-json` (config only, never hot path).

---

## 4. The plugin ABI — freeze this early, change it never

This is the most important interface in the system. Get it right at M4 and treat it as frozen.
Any change after M4 is a **STOP AND ASK**.

### 4.1 `include/marketscan/abi/plugin_abi.h` — pure C

Implement it substantially as written. You may add fields, but every struct must stay POD,
every function pointer must be `extern "C"`, and nothing may throw across the boundary.

```c
#ifndef MARKETSCAN_PLUGIN_ABI_H
#define MARKETSCAN_PLUGIN_ABI_H

#include <stdint.h>
#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MS_ABI_VERSION 1u

typedef int64_t  MS_Nanos;      /* nanoseconds since Unix epoch, UTC */
typedef int64_t  MS_Money;      /* micro-dollars: 1 USD == 1000000 */
typedef uint32_t MS_SymbolId;

typedef enum { MS_SIDE_BUY = 0, MS_SIDE_SELL = 1 } MS_Side;
typedef enum { MS_ORD_MARKET = 0, MS_ORD_LIMIT = 1 } MS_OrderType;
typedef enum { MS_TIF_DAY = 0, MS_TIF_IOC = 1, MS_TIF_GTC = 2 } MS_TimeInForce;
typedef enum { MS_LOG_DEBUG=0, MS_LOG_INFO=1, MS_LOG_WARN=2, MS_LOG_ERROR=3 } MS_LogLevel;

typedef struct { MS_Nanos ts; MS_SymbolId symbol;
                 double open, high, low, close, volume; } MS_Bar;

typedef struct { MS_Nanos ts; MS_SymbolId symbol;
                 double bid_px, ask_px, bid_sz, ask_sz; } MS_Quote;

typedef struct { MS_Nanos ts; MS_SymbolId symbol;
                 double price, size; MS_Side aggressor; } MS_Trade;

typedef struct { MS_Nanos ts; MS_SymbolId symbol; uint64_t order_id;
                 MS_Side side; double price, quantity;
                 MS_Money commission; } MS_Fill;

typedef struct {
    MS_SymbolId    symbol;
    MS_Side        side;
    MS_OrderType   type;
    MS_TimeInForce tif;
    double         quantity;
    double         limit_price;      /* ignored when type == MS_ORD_MARKET */
    uint64_t       client_order_id;  /* strategy-assigned, echoed back on fill */
} MS_OrderRequest;

/* Services the host provides to the plugin. The plugin must use these and
   nothing else for time, randomness, order flow, and account state. */
typedef struct MS_HostContext {
    void* host;   /* opaque host handle, pass back as first arg */

    uint64_t (*submit_order)(void* host, const MS_OrderRequest* req);
    void     (*cancel_order)(void* host, uint64_t order_id);

    MS_Nanos (*now)(void* host);
    double   (*random_uniform)(void* host);          /* seeded, deterministic */

    double   (*position)(void* host, MS_SymbolId s); /* signed units */
    MS_Money (*cash)(void* host);
    MS_Money (*equity)(void* host);

    MS_SymbolId (*symbol_id)(void* host, const char* ticker);
    const char* (*symbol_name)(void* host, MS_SymbolId s);
    uint32_t    (*symbol_count)(void* host);

    void (*log)(void* host, MS_LogLevel lvl, const char* msg);
    /* publish a named scalar; the GUI will plot it automatically */
    void (*publish_metric)(void* host, const char* name, double value);
} MS_HostContext;

typedef enum { MS_PARAM_DOUBLE=0, MS_PARAM_INT=1,
               MS_PARAM_BOOL=2, MS_PARAM_ENUM=3 } MS_ParamType;

typedef struct {
    const char*  name;
    const char*  description;
    MS_ParamType type;
    double       min_value, max_value, default_value;
    const char* const* enum_labels;   /* NULL unless MS_PARAM_ENUM */
    uint32_t     enum_count;
} MS_ParamDesc;

typedef struct {
    uint32_t     abi_version;         /* must equal MS_ABI_VERSION */
    const char*  id;                  /* stable unique id, e.g. "sma_crossover" */
    const char*  display_name;
    const char*  author;
    const char*  description;
    const MS_ParamDesc* params;
    uint32_t     param_count;
    const char* const*  required_symbols;  /* NULL == subscribe to all */
    uint32_t     required_symbol_count;
} MS_StrategyDesc;

/* Instance vtable. Any callback may be NULL if the strategy does not need it. */
typedef struct {
    void   (*destroy)   (void* self);
    void   (*on_start)  (void* self, const MS_HostContext* host);
    void   (*on_bar)    (void* self, const MS_Bar* bar);
    void   (*on_quote)  (void* self, const MS_Quote* quote);
    void   (*on_trade)  (void* self, const MS_Trade* trade);
    void   (*on_fill)   (void* self, const MS_Fill* fill);
    void   (*on_stop)   (void* self);
    void   (*set_param) (void* self, const char* name, double value);
    double (*get_param) (void* self, const char* name);
} MS_StrategyVTable;

/* --- The three symbols every plugin DLL must export --- */
MS_EXPORT uint32_t               ms_plugin_abi_version(void);
MS_EXPORT const MS_StrategyDesc* ms_plugin_describe(void);
MS_EXPORT void*                  ms_plugin_create(MS_StrategyVTable* out_vtable);

#ifdef __cplusplus
}
#endif
#endif
```

### 4.2 The C++ ergonomic layer

Nobody should have to write C to author a theory. `istrategy.hpp` provides an abstract base
class with virtual `on_bar` / `on_quote` / `on_fill` etc. taking the same POD structs, plus a
macro that generates the three C exports and the vtable shims:

```cpp
class MyTheory : public marketscan::IStrategy { /* ... */ };
MARKETSCAN_EXPORT_STRATEGY(MyTheory, "my_theory", "My Theory", "Clark Kirby")
```

The macro must wrap every shim in a `try/catch(...)` that logs and swallows — an exception must
never unwind across the C boundary.

### 4.3 Loader requirements (`plugin_loader.hpp`)

- Scan `theories/` recursively for `*.dll` at startup and on demand.
- **Windows gotcha:** `LoadLibrary` locks the DLL file, so a rebuild of the plugin fails while
  the app holds it. Copy each discovered DLL to a per-session shadow directory under
  `%TEMP%/marketscan/<pid>/` and load the copy. This is what makes hot reload work.
- Check `ms_plugin_abi_version()` **before** calling anything else. Mismatch → refuse to load,
  surface a clear error in the GUI, do not crash.
- A plugin missing any of the three exports → refuse, clear error.
- A plugin that throws, returns null, or crashes during `create` → refuse, clear error, keep
  the app alive. Wrap load attempts in SEH (`__try/__except`) on MSVC.
- Watch `theories/` for changes (ReadDirectoryChangesW or poll mtime at 1 Hz) and surface a
  "reload available" badge in the GUI. Reload must be user-triggered, never automatic mid-run.
- Unload must call `on_stop`, `destroy`, then `FreeLibrary`, and must be safe while the engine
  is running.

---

## 5. Core engine design

**Clock.** `IClock` with `now()`. `SimulatedClock` is advanced by the replay driver to each
event's timestamp. `SystemClock` reads the OS. Strategies see only `host->now()`. This single
abstraction is what makes backtest and live the same code.

**Event loop.** Single-threaded in replay, for determinism. In live mode a feed thread pushes
into the lock-free SPSC ring buffer and the engine thread drains it — the engine body is
identical in both cases. Ring buffer slots must be cache-line aligned (`alignas(64)`) with the
producer and consumer indices on separate lines to avoid false sharing.

**Multi-strategy.** The engine holds N strategy instances. Every instance receives every event
it subscribed to, in timestamp order, and owns its own `Ledger`. Instances never see each
other. At v1 frequencies, dispatching synchronously to all instances per event is fine — do not
add threading here.

**Ledger** (per strategy): cash (`MS_Money`), positions, realized and unrealized P&L, full trade
blotter, and an equity time series sampled at a configurable interval. Invariant that must hold
after every event and be asserted in tests:
`equity == cash + Σ(position_i × mark_price_i)`.

**Fill models** (`IFillModel`):

- `SpreadCrossingFillModel` — **the default.** Market buys fill at the ask, sells at the bid,
  using the quote in effect at the signal timestamp. Orders larger than the touch size walk the
  book to worse levels. Configurable per-share commission and slippage in basis points.
- `OptimisticFillModel` — fills everything at the last trade price, instantly, in full. Exists
  **only** so the GUI can show the two side by side and make the cost of naive assumptions
  visible. Must be labeled in the UI as unrealistic.
- Design `IFillModel` so a future `QueuePositionFillModel` slots in without touching the
  engine. Do not implement it now — it needs MBO data we don't have.

**Latency instrumentation.** Wrap the event-in → order-out path with a steady_clock or `rdtsc`
measurement into a histogram. Report p50/p90/p99/p99.9. In replay the numbers are meaningless
as trading latency, but the instrumentation must exist and be correct from day one.

**Determinism contract.** Running the same input twice must produce a byte-identical order log.
`test_determinism.cpp` enforces it: run a replay twice, hash the serialized order stream,
assert equality. This test failing is always a real bug — never quarantine it.

---

## 6. The two reference theories

These exist to prove the plugin system works and to calibrate the simulator. They are the
**only** strategies in this build.

**`buy_and_hold`** — buys the configured symbol with all available cash on the first bar and
holds. This is the benchmark, and implementing it as a plugin rather than special-casing it in
the host is deliberate: the benchmark then pays the same spread, runs through the same ledger,
and is compared apples-to-apples by construction. The GUI treats it as the default benchmark but
it is otherwise an ordinary theory.

**`sma_crossover`** — fast/slow simple moving average crossover, parameters exposed via
`MS_ParamDesc` so the GUI renders sliders automatically. This is a **calibration instrument, not
a strategy.** On realistic fills it should come out flat to modestly negative after costs. If it
backtests profitably, the fill simulator is wrong. Put that sentence in its README.

---

## 7. GUI

Dear ImGui (docking branch) + ImPlot, GLFW + OpenGL3 backend. Dark theme, custom accent, rounded
frames, a clean sans font loaded from file (Inter or JetBrains Mono — commit the TTF). Layout
persisted to `imgui.ini` next to the executable, with a "Reset Layout" menu item.

Panels, each in its own file under `apps/marketscan/panels/`:

1. **Leaderboard** — sortable table, one row per loaded theory: display name, equity, day P&L,
   total return, Sharpe, max drawdown, trade count, information ratio vs. benchmark. Click a row
   to focus it everywhere else.
2. **Equity Curves** — all theories overlaid plus the benchmark, normalized to the same starting
   capital. Toggle series on/off from the legend. Log-scale toggle.
3. **Drawdown** — underwater plot, selected theory vs. benchmark, shaded fill.
4. **Relative Strength** — selected theory ÷ benchmark, so flat stretches are visible.
5. **Correlation Matrix** — heatmap of pairwise return correlation across loaded theories.
   Build this even though it is near-useless with two reference theories; it is load-bearing
   later and the layout should be settled now.
6. **Trade Blotter** — filterable, sortable table of every fill: timestamp, theory, symbol,
   side, quantity, requested price, fill price, **slippage**, commission. Slippage gets its own
   column because separating "the signal was wrong" from "I couldn't get filled" is the whole
   point.
7. **Positions & P&L** — current positions, average cost, mark, unrealized, realized, cash.
8. **Theory Detail** — drill-down for the selected theory: its own equity curve, its published
   metrics (from `publish_metric`), and **live parameter sliders** generated from its
   `MS_ParamDesc` array.
9. **Plugin Manager** — every discovered DLL: path, id, author, ABI version, load state, and any
   load error verbatim. Buttons to load, unload, reload, and rescan. Errors must be readable
   here, not only in the log.
10. **Transport** — play, pause, step-one-event, step-one-bar, speed multiplier (1×…10000×,
    plus "as fast as possible"), and a scrub bar over the loaded dataset. Scrubbing backward
    re-runs from the start to the target timestamp; it must be deterministic and it must show
    progress.
11. **Latency** — histogram plus p50/p99/p99.9 readout.
12. **Log Console** — spdlog sink, level filter, per-theory filter, autoscroll toggle.

Ergonomics that matter: every table sortable and column-resizable, keyboard shortcuts for
transport (space = play/pause, right arrow = step), no modal dialogs blocking the render loop,
and 60fps maintained while the engine runs at max speed. Do not block the UI thread on the
engine — run the engine on its own thread and publish snapshots the GUI reads.

---

## 8. Milestones

Commit at every Definition of Done with a clear message. Push to `origin/main`.

| # | Milestone | Definition of Done |
|---|---|---|
| **M0** | Repo skeleton | CMake + vcpkg manifest + presets; `.gitignore`, `.clang-format`, MIT LICENSE, README stub; CI builds a hello-world on `windows-latest` and is green; remote wired and pushed |
| **M1** | Core types & engine loop | `Money` fixed-point with tests; `IClock`/`SimulatedClock`/`SystemClock`; `MarketEvent`; SPSC ring buffer with tests; engine dispatches events in timestamp order |
| **M2** | Data feeds | Seeded synthetic GBM feed; CSV feed; committed sample CSV; replay produces identical event sequences across runs |
| **M3** | Ledger & fills | `Ledger` with the equity invariant asserted in tests; `SpreadCrossingFillModel` + `OptimisticFillModel`; tests prove market orders always cross and never fill better than the touch |
| **M4** | **Plugin ABI & loader** | `plugin_abi.h` final; C++ base class + export macro; loader with shadow-copy, ABI check, graceful failure; `_template` builds standalone; `buy_and_hold` loads from `theories/` and trades. **ABI is frozen after this point.** |
| **M5** | Metrics | CAGR, vol, Sharpe, Sortino, max DD, Calmar, beta, alpha, information ratio, correlation; rolling variants; tests against hand-computed known inputs |
| **M6** | GUI shell | Window, docking, theme, font, layout persistence, log console, one working equity-curve panel rendering live engine output at 60fps |
| **M7** | Full GUI | All twelve panels; transport controls; plugin manager with hot reload working end to end |
| **M8** | Python bindings | pybind11 module exposing engine, feeds, ledger, metrics; headless backtest runnable from a notebook; `quickstart.ipynb` committed and executed |
| **M9** | Hardening & polish | Determinism regression test in CI; `sma_crossover` reference theory; `theories/README.md` authoring guide; root README with screenshots and build instructions |

Testing is not a milestone — it is part of every milestone. Catch2, run under `ctest`, wired
into CI from M0.

---

## 9. STOP AND ASK triggers

Stop, state the problem and the options you see, and wait for an answer. Do not guess on these.

1. Any change to `plugin_abi.h` after M4 is frozen — including "just adding a field."
2. Any situation where the cleanest fix would make backtest and live paths differ.
3. A test that cannot be made deterministic, or `test_determinism` failing for a reason you
   cannot explain.
4. A vcpkg dependency that will not resolve on MSVC and would require swapping to a different
   library than the ones named in Section 3.
5. Any ambiguity about financial semantics: dividend handling, splits, halts, partial fills,
   short-sale mechanics, margin, overnight position marking. These are modeling decisions with
   real consequences — do not invent one silently.
6. Anything that would require a network call, an API key, a paid service, or a login.
7. A milestone that is going to run more than ~2× the scope you estimated, or that you think is
   sequenced wrong.
8. Any place where you believe this specification is internally inconsistent or wrong. Say so
   plainly rather than picking an interpretation.

Batch questions where you can — if you hit three small ambiguities in M5, ask all three at once
rather than stopping three times.

---

## 10. What "done" looks like

A single `marketscan.exe` that launches to a dark, docked, professional-looking dashboard.
Two reference theories load automatically from `theories/`. Pressing space replays the sample
dataset; equity curves draw in real time; the leaderboard sorts; the blotter fills with trades
showing honest slippage; SMA crossover ends the run slightly down after costs; buy-and-hold ends
wherever the market ended. Dropping a freshly-built DLL into `theories/` and clicking rescan
makes a third strategy appear without restarting. `ctest` is green. CI is green.

No trading theory in the repo does anything a reasonable person would call a strategy — and
that is the point. The car is finished. The engine bay is empty and every mount is standard.
