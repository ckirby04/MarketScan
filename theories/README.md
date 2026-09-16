# Writing a theory

A **theory** is a trading strategy compiled into a DLL. Drop it in this folder,
press Rescan, and the host loads it, gives it its own virtual account, and puts
it on the leaderboard. No host code changes. No recompiling the application.

That is the whole idea: `theories/` is a USB port.

---

## The shortest possible version

```powershell
# 1. Copy the skeleton
cp -r theories/_template theories/my_theory

# 2. Build it
./scripts/dev-env.ps1 cmake --preset release
./scripts/dev-env.ps1 cmake --build --preset release

# 3. It is already in out/build/release/bin/theories/. Press Rescan.
```

Or build it entirely outside this repository:

```powershell
cmake -S theories/my_theory -B build -DMARKETSCAN_ROOT=C:/path/to/MarketScan
cmake --build build --config Release
# then copy the .dll into the host's theories/ directory
```

> ⚠️ **Build your theory with the same preset as the host.** MSVC's debug and
> release runtimes use different `std::string` and `std::vector` layouts. A
> Debug plugin loaded by a Release host does not fail cleanly — it corrupts
> memory in ways that take days to find. This is also why the plugin boundary
> itself is pure C.

---

## The smallest theory that does something

```cpp
#include "marketscan/strategy/istrategy.hpp"

namespace {

class MyTheory final : public marketscan::IStrategy {
public:
    void on_bar(const MS_Bar& bar) override {
        if (position(bar.symbol) == 0.0) {
            submit_market(bar.symbol, MS_SIDE_BUY, 10.0);
        }
    }
};

}  // namespace

MARKETSCAN_EXPORT_STRATEGY(MyTheory, "my_theory", "My Theory", "Your Name")
```

That is the entire contract. `MARKETSCAN_EXPORT_STRATEGY` generates the three C
entry points the host looks for and a vtable whose every entry catches
everything, so an exception in your code cannot unwind across the boundary and
take the application with it.

---

## Rules

Three of these are hard. Breaking one does not produce an error message; it
produces a backtest that cannot be reproduced, which is worse.

| Don't | Do | Why |
|---|---|---|
| `std::chrono::system_clock::now()`, `time()` | `now()` | In replay the host hands you the event's timestamp. A strategy that reads the wall clock behaves differently every run. |
| `rand()`, your own seeded generator | `random_uniform()` | The host's stream is seeded per run *and* per theory, so your draws do not change when someone loads an unrelated theory. |
| File or network I/O in a callback | Take it as a parameter | A callback that blocks stalls the engine; one that reads changing state is not replayable. |

Two more that are about honesty rather than mechanics:

- **You cannot ask what the current quote is.** There is no such call in the
  ABI, deliberately. A strategy is a function of the events it was shown. If
  you need the spread, remember it from `on_quote`.
- **Your fills will be worse than your signals.** A market buy pays the ask and
  a market sell receives the bid, always. `fill.price` is what you actually
  got; the difference from what you decided on is the slippage column in the
  blotter, and it is usually where the edge went.

---

## What you get

Everything below is available from inside `IStrategy`.

**Market data** — override the callbacks you want. All are optional.

```cpp
void on_start() override;                  // before the first event
void on_bar(const MS_Bar&) override;
void on_quote(const MS_Quote&) override;
void on_trade(const MS_Trade&) override;
void on_fill(const MS_Fill&) override;     // one of your orders traded
void on_stop() override;                   // after the last event
```

**Orders**

```cpp
uint64_t submit_market(symbol, MS_SIDE_BUY, quantity);
uint64_t submit_limit(symbol, MS_SIDE_SELL, quantity, limit_price, MS_TIF_GTC);
void     cancel(order_id);
```

`submit_*` returns the engine's order id, or **0 if the order was refused**.
Check it. The common refusal is `InsufficientCash`: this is a cash account, and
an order it cannot pay for is refused whole rather than filled part way.

**Account**

```cpp
double   position(symbol);   // signed; negative is short
MS_Money cash();             // micro-dollars: 1 USD == 1'000'000
MS_Money equity();
```

**Instruments**

```cpp
MS_SymbolId symbol_id("SPY");    // 0 if this run has no such instrument
const char* symbol_name(id);
uint32_t    symbol_count();
```

**Telling the user things**

```cpp
log_info("something happened");
publish_metric("signal_strength", value);   // the GUI plots this by itself
```

---

## Parameters

Declare them and bind them. The GUI renders a control for each one and lets the
user move it while the run is going.

```cpp
class MyTheory final : public marketscan::IStrategy {
public:
    static constexpr MS_ParamDesc kParams[] = {
        marketscan::param_int("fast", "Fast moving average length.", 2, 200, 10),
        marketscan::param_int("slow", "Slow moving average length.", 5, 500, 50),
        marketscan::param_bool("allow_shorts", "Trade the short side too.", false),
    };
    static constexpr uint32_t kParamCount = 3;

    MyTheory() {
        bind_param("fast", &fast_);
        bind_param("slow", &slow_);
        bind_param("allow_shorts", &allow_shorts_);
    }

private:
    int fast_ = 10;
    int slow_ = 50;
    bool allow_shorts_ = false;
};
```

Binding is all you need — the base class implements `set_param` and `get_param`
against the bindings. Override them only for something a binding cannot express,
such as a value that has to invalidate a cache when it changes.

**Every parameter is a `double` on the wire.** There is no way to pass a ticker
as a parameter; take an index into the symbol table instead, as `buy_and_hold`
does, or declare `kRequiredSymbols`.

### Other optional metadata

```cpp
static constexpr const char* kDescription = "What this theory believes.";

static constexpr const char* kRequiredSymbols[] = {"SPY", "QQQ"};
static constexpr uint32_t kRequiredSymbolCount = 2;
```

Declare `kRequiredSymbols` to be given only those instruments. Leave it out and
you get every symbol in the run. A required ticker the dataset does not contain
is logged once, at startup, rather than leaving you to work out why the blotter
is empty.

---

## When it does not load

The Plugin Manager panel shows every DLL it found and, for the ones it refused,
the reason verbatim. The usual ones:

| Message | Cause |
|---|---|
| `missing export(s) …` | `MARKETSCAN_EXPORT_STRATEGY` is missing, is inside a namespace, or the DLL was built without `MARKETSCAN_PLUGIN_BUILD`. Use `marketscan_add_theory()` and it is handled. |
| `ABI version mismatch` | Built against a different version of `plugin_abi.h`. Rebuild. |
| `LoadLibrary failed` | Not a DLL, the wrong architecture (must be x64), or a missing dependency of your own. |
| `ms_plugin_create returned null` | Your constructor threw. The macro catches it and reports rather than crashing the host. |
| `descriptor has an empty id` | The id passed to the macro was blank. |

A plugin that fails never takes the application down, and never stops the other
theories loading.

---

## Hot reload

The host never loads the DLL you built. It copies it to a private directory and
loads the copy, because Windows locks a loaded DLL and you would otherwise get
a link error from your own rebuild.

So the loop is: edit, rebuild, press **Rescan**. The Plugin Manager raises a
badge when it notices a file has changed.

Reload is never automatic. Swapping a strategy's code out from under a running
replay would silently invalidate every number on screen.

---

## The two reference theories

Neither is a strategy. They exist to prove the plumbing works and to calibrate
the simulator.

**`buy_and_hold`** — buys one instrument on the first bar and holds it. This is
the default benchmark, and it is a plugin rather than a special case in the host
on purpose: as a plugin it pays the same spread, the same commission and runs
through the same ledger as whatever it is being compared against. A benchmark
implemented inside the host would be none of those things, and every "we beat
the benchmark" claim measured against it would really be comparing two execution
models.

**`sma_crossover`** — a fast/slow moving average crossover. **This is a
calibration instrument, not a strategy.** On realistic fills it should come out
flat to modestly negative after costs. If it backtests profitably, the fill
simulator is wrong.
