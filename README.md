# MarketScan

A quantitative trading research platform in C++20, built around one idea:

> **A strategy cannot tell whether it is being backtested, paper traded, or trading live.**

Same strategy binary, same engine, same ledger, same fill simulator. Only the *clock* and the
*transport* swap out. There is no "backtest mode" code path to diverge from production.

The second idea is the `theories/` folder as a **USB port**: a compiled strategy plugin dropped
into it is discovered at runtime, loaded, given its own virtual account, and shown in the GUI —
with no recompile of the host and no changes to host code.

> **Status: early.** The engine chassis is under construction; see [Milestones](#milestones).
> This repository deliberately contains **no real trading strategies**.

---

## Why the design looks like this

| Decision | Reason |
|---|---|
| Pure **C** plugin ABI | MSVC debug and release CRTs have incompatible STL layouts. Passing `std::string` across a DLL boundary corrupts memory in ways that take days to find. |
| `int64` micro-dollars for cash | Binary floating point cannot represent `0.01`. Cash accounting must be exact; prices may be `double`. |
| Time only via `host->now()` | A strategy that reads the wall clock is not replayable, and a backtest you cannot reproduce is not evidence. |
| Seeded RNG from the host | Same reason. Determinism is a test target, not an aspiration. |
| Fills cross the spread **by default** | An optimistic fill model turns noise into alpha. The optimistic model exists only as a labelled comparison baseline. |
| Benchmark is a plugin, not a special case | Buy-and-hold pays the same spread and runs through the same ledger, so the comparison is apples-to-apples by construction. |

---

## Building

**Requirements**

- Windows 11, Visual Studio 2022 (MSVC v143, x64 C++ toolset)
- CMake ≥ 3.25 and Ninja — the copies bundled with Visual Studio are used automatically
- [vcpkg](https://github.com/microsoft/vcpkg) — a **full** clone (not `--depth 1`; vcpkg needs
  history to resolve versions)

```powershell
git clone https://github.com/microsoft/vcpkg.git $env:USERPROFILE\vcpkg
& $env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat
```

The build finds vcpkg via `VCPKG_ROOT`, then `%USERPROFILE%\vcpkg`, then a few other
conventional locations, so no environment setup is normally required.

**Build and test**

```powershell
git clone https://github.com/ckirby04/MarketScan.git
cd MarketScan

./scripts/dev-env.ps1 cmake --preset release
./scripts/dev-env.ps1 cmake --build --preset release
./scripts/dev-env.ps1 ctest --preset release
```

`scripts/dev-env.ps1` enters an MSVC developer environment before running the command it is
given. Run it once per command, as above.

**Presets**

| Preset | Contents |
|---|---|
| `debug` | Full desktop build, assertions on |
| `release` | Full desktop build, optimised |
| `full` | `release` plus the Python bindings |
| `ci` | Headless engine, theories and tests — no GUI dependency stack |

> ⚠️ A theory plugin **must** be built with the same preset as the host that loads it. Mixing
> Debug and Release binaries across the plugin boundary is undefined behaviour on MSVC.

---

## Python

```powershell
./scripts/dev-env.ps1 cmake --preset full
./scripts/dev-env.ps1 cmake --build --preset full
```

```python
import sys; sys.path.insert(0, "out/build/full/bin")
import marketscan as ms

result = ms.run(theories=["out/build/full/bin/theories"],
                data="data/samples/synthetic_spy_1min.csv")

for theory in result.theories:
    print(theory.display_name, theory.final_equity, theory.summary.sharpe)
```

The bindings call the same `run_backtest` the command line does, so a notebook
and the desktop application cannot disagree about what happened.

See [`bindings/python/examples/quickstart.ipynb`](bindings/python/examples/quickstart.ipynb),
which is committed with its output.

**Strategies are not written in Python.** The engine's contract is that a
strategy is a pure function of the events it was shown; a Python callback on the
per-event path would make replay speed depend on the interpreter and let a
strategy close over state the engine cannot see. Strategies are DLLs — see
[`theories/README.md`](theories/README.md).

The module targets whichever Python minor version vcpkg's `python3` port
provides (currently 3.12). Override with
`-DMARKETSCAN_PYTHON_EXECUTABLE=<path>` to build against a specific interpreter
of that same version.

---

## Milestones

| | Milestone | State |
|---|---|---|
| M0 | Repo skeleton, CMake + vcpkg + CI | ✅ |
| M1 | Core types and engine loop | ✅ |
| M2 | Data feeds | ✅ |
| M3 | Ledger and fill models | ✅ |
| M4 | Plugin ABI and loader | ✅ |
| M5 | Metrics | ✅ |
| M6 | GUI shell | ✅ |
| M7 | Full GUI | ✅ |
| M8 | Python bindings | ✅ |
| M9 | Hardening and polish | ⬜ |

---

## Repository layout

```
include/marketscan/   public headers
  abi/                the frozen pure-C plugin boundary
  core/               types, money, clock, events, ring buffer, engine
  data/               market data feeds
  book/               order book
  exec/               orders, fill models, ledger
  strategy/           strategy base class, registry, plugin loader
  metrics/            performance statistics
  util/               logging, latency histogram
src/                  implementation, mirroring include/
apps/marketscan/      the desktop application
theories/             the USB port: strategy plugins, discovered at runtime
bindings/python/      pybind11 module
tests/                Catch2 suite
data/samples/         small committed datasets for tests and first run
```

---

## License

MIT — see [LICENSE](LICENSE).
