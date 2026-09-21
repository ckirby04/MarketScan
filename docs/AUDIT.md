# Chassis audit

Spec 2, M0. Every check the specification asks for, with its result and the
evidence behind it. Findings are recorded whether or not they are flattering;
a clean audit that was produced by not looking is worth nothing.

Audited at `2535ca2` (the tip of Spec 1). Host: Windows 11, MSVC 19.43, x64.

**Summary: 10 checks, 8 pass, 2 pass with findings. 4 gaps closed with new
tests, 2 findings escalated to the reader rather than fixed.**

> **Updated at M1b.** Escalation 2 — the backtest/live seam — is resolved; see
> check 5.
>
> **Updated at the Session A close-out.** Escalation 1 — the specifications
> were not in the repository — is resolved. They are in
> [`specs/`](specs/), verbatim. **Both escalations from this audit are
> closed.**
>
> **Session F added a second audit**, with a narrower question: which tests would
> pass while measuring nothing. It is at the bottom of this file, and it found
> thirteen.

---

## 1. The plugin ABI contains no C++ types — **PASS**

`include/marketscan/abi/plugin_abi.h` includes `<stdint.h>` and `abi/export.h`
and nothing else. Grepping for `std::`, `class`, `template` and `throw`
returns exactly one hit, and it is in a comment explaining why none of them
are there. Everything between `extern "C" {` and `}` is C.

One observation, not a failure. The layout guards at the foot of the file are
C++ `static_assert`s inside `#ifdef __cplusplus`, so a consumer compiling the
header as C gets no guard at all. C11 has `_Static_assert` and the guards
could be written to use it. Nothing today compiles this header as C, so this
is noted rather than changed.

## 2. Cash and realized P&L are `int64` micro-dollars — **PASS**

`Money` wraps a single `std::int64_t` of micro-dollars. `Ledger` holds
`starting_cash_`, `cash_`, `realized_pnl_` and `commission_paid_` as `Money`,
and `Position::cost_basis` is `Money` rather than a floating average cost.

`double` appears in the ledger in exactly two roles, both permitted by the
specification: prices and quantities, and the `to_double()` / `average_cost()`
conversions that exist for display. No accumulator is a `double`.

## 3. The default fill model crosses the spread — **PASS**

Traced through construction rather than read off a comment. There are two
places a fill model is built:

| Site | Default |
|---|---|
| `run/backtest.cpp:88` | `optimistic_fills` defaults `false` → `SpreadCrossingFillModel` |
| `apps/marketscan/session.cpp:380` | `SessionConfig::optimistic_fills` defaults `false` → same |

`Broker` takes the model as a borrowed pointer with no default argument, so
there is no third path that could quietly construct the optimistic one.

## 4. No hash-container iteration on an order-emitting path — **PASS**

One `std::unordered_map` exists in the whole engine, in `csv_feed.cpp`: a
header-alias table consulted with `.find()` and never iterated, with a comment
at the call site saying so. `SymbolTable` uses `std::map` deliberately, and
says why in its header.

## 5. No second code path for backtest versus live — **PASS; finding since RESOLVED**

The literal check passed at M0. There is no `is_backtest`, no mode flag, no
`#ifdef` that changes engine behaviour; the engine cannot tell.

**The finding, as recorded at M0.** The seam was a claim rather than a socket.
`Engine` held a `SimulatedClock` **by value**, not an `IClock&`, and
`SystemClock` existed but was used only by the logger. "Only the clock swaps"
could not be demonstrated, because there was nothing to swap it with and
nowhere to put it. The judgment at the time was not to add a virtual call to
the per-event path for a capability nothing used.

**Resolved at M1b**, and by a route the M0 audit did not consider. The engine
is now a template on its clock:

```cpp
template<typename Clock> class EngineT { ... };
using Engine     = EngineT<SimulatedClock>;
using LiveEngine = EngineT<SystemClock>;
```

Both are explicitly instantiated in `engine.cpp`, so both are compiled on every
build. There is no virtual call on the event path and no runtime cost at all;
the price is an explicit instantiation per clock type, which is one line.
`SystemClock` gained `advance_to` and `reset` as documented no-ops — real time
does not take instruction — and that is what makes one event loop compile
against both.

It is also *run*, not merely instantiated: `the same engine runs on a wall
clock` drives a `LiveEngine` over a canned source and asserts the loop behaves
identically while the clock genuinely differs — a `SimulatedClock` would be
sitting on the last event's timestamp, and this one is in this century.

The M0 escalation went further than the clock, and correctly. The larger half
of that seam was the **order lifecycle**, and it is what M1b was really about;
see `docs/EXECUTION.md`.

## 6. No allocation, lock or syscall in the per-event hot path — **PASS, measured**

This was the check that could not be settled by reading, so it is now
measured. `tests/test_hot_path.cpp` replaces the global `operator new` and
counts allocations inside an armed region. Two calibration tests prove the
counter itself works before anything is claimed with it.

Measured over 4,679 dispatched events, built with the `ci` preset (optimised;
the debug allocator adds bookkeeping allocations of its own and would measure
the compiler rather than the code):

| Path | Allocations | Per event |
|---|---|---|
| Engine dispatch with a counting sink | **0** | 0 |
| Full strategy path, 117 trades, broker + fill model + ledger | **46** | 0.0098 |

The engine's dispatch path — select the next source, advance the clock, apply
to the book, hand to every sink — allocates **exactly zero times**, asserted.

The strategy path allocates 46 times across 4,679 events. Those are the
geometric growth steps of the containers that have to keep what outlives the
event: the order list, the blotter, the equity curve. That is not asserted to
be zero, because a blotter recording a fill has to put it somewhere and a test
claiming otherwise would be a test that lies. What is asserted is that the
cost is amortised — a second pass over the identical events allocates strictly
fewer times than the first, because by then nothing needs to grow.

No mutex is taken and no syscall is made on the event path. The GUI's
`Session` locks, but it does so around building and tearing down a run, not
around dispatch.

## 7. The per-strategy RNG was never exercised — **FIXED**

Confirmed: neither `buy_and_hold` nor `sma_crossover` calls `random_uniform()`,
so through all of Spec 1 the derived-stream machinery was plumbed, documented,
and never once run.

Closed with `tests/plugins/dice_roller.cpp`, a fixture plugin that draws from
the host stream and publishes a position-sensitive digest of what it got, plus
three tests:

- **a theory's random draws replay exactly** — same seed, same draws.
- **a different run seed gives a different stream** — the guard against a
  generator that ignores its seed.
- **two theories drawing at once do not perturb each other** — the one that
  costs something. Alice draws the same numbers whether or not Bob is in the
  room drawing on every bar. The test first asserts Alice and Bob get
  *different* streams, so the property cannot pass trivially.

A single shared generator would satisfy the first two and fail the third,
which is exactly the failure worth catching: every run still reproducible,
every result silently dependent on what else was loaded.

## 8. `MARKETSCAN_WERROR` was off — **FIXED, zero warnings found**

It was off for M0–M8 of Spec 1 and was turned on for the two `ci` presets at
`4828dee`. For this audit every preset was configured with `-DMARKETSCAN_WERROR=ON`
and built from scratch:

| Preset | Warnings at `/W4 /WX` |
|---|---|
| `ci` | 0 |
| `ci-debug` | 0 |
| `release` (GUI: ImGui, ImPlot, GLFW) | 0 |
| `full` (GUI + pybind11) | 0 |

**Accumulated warning count: zero.** Nothing needed fixing.

The desktop presets are still left permissive on purpose: a developer halfway
through a change should be able to build with an unused variable in the file
they are editing. What they should not be able to do is merge it, and CI now
stops that. Since the count is zero, enabling it everywhere is a one-line
change if that trade is ever judged the wrong way round.

## 9. `full` runs one test more than every other preset — **EXPLAINED**

Diffed the two test sets from `ctest --show-only=json-v1`. The difference is
one test:

```
python_bindings
```

It imports the built `.pyd` and runs `bindings/python/examples/smoke_test.py`
against it. It cannot exist in a preset that does not build the module, so
this is correct and not a registration bug. Written into the `full` preset's
description so the next person does not have to diff it again.

## 10. Milestone commit messages versus what the tests assert — **PASS, with one finding**

Every quantitative claim in the M0–M9 commit messages was checked against the
test that is supposed to back it.

| Claim | Where | Verdict |
|---|---|---|
| "test_money proves a double accumulator drifts" | M1 | True — the test asserts `naive != 10'000.0` as well as the fixed-point result |
| "Tie-breaking is registration order… both are tested" | M1 | True — two named tests |
| "rejects an out-of-order event" | M1 | True — `an out-of-order source is rejected` |
| "reset asks every source before rewinding any" | M1 | True — tested, including the refusal case |
| "rejects impossible bars and crossed quotes" | M2 | True — both are sections of `malformed input is refused` |
| "CsvFeed sorts stably" | M2 | True — `rows are sorted by timestamp, stably` |
| "tested across 2000 random sizes per side" | M3 | True — 2,000 iterations, each testing a buy and a sell |
| "across 20000 randomised fills" | M3 | True — the loop and the blotter assertion both say 20,000 |
| "the script reproduces the file byte for byte" | M2 | **True but undefended — see below** |
| "a second theory must not move the first one's numbers" | M9 | Already known false when written; the test was added afterwards at `fe279d3` |

**The finding.** M2 claims `scripts/make_sample_data.py` reproduces
`data/samples/synthetic_spy_1min.csv` byte for byte. It does — verified by
regenerating and comparing SHA-256, which matched at
`a2a94eee755a59f9…`, 52,724 bytes. But nothing defended the claim. The script
is not run by the build, so a change to `SyntheticFeed` would leave the
generator and the committed file silently disagreeing, and that file is an
input to several tests: it would have become a fossil of a generator that no
longer exists.

Closed from the C++ side, which needs no Python on the machine: **the
committed sample is what the generator produces** regenerates the sample with
the configuration the script documents and compares all 780 bars against the
committed file — 6,243 assertions.

---

## Findings not covered by a numbered check

**`BUILD_SPEC.md` is not in the repository.** *(Resolved at the Session A
close-out.)* Spec 2 names it as prerequisite
reading for its Section 2 and Section 4. It has never been committed; Spec 1
was delivered as a message and only its conclusions reached disk, in code
comments, in `README.md` and in the commit messages. The constraints are being
honoured, but a reader cannot check them against the source, and neither can a
future session. Reconstructing it from memory would produce a document that
looks authoritative and is not, so it has not been attempted. Flagged for the
author to commit the original.

**Closed.** The original documents were placed in the working tree and are now
committed under [`specs/`](specs/) — `BUILD_SPEC.md`, `BUILD_SPEC_2.md`,
`SPEC_2_M1B.md` and `SPEC_VIEW_MODES.md` — unedited, with a README noting where
`BUILD_SPEC_2.md` §6 has been superseded by the identity model in
[`IDENTITY.md`](IDENTITY.md).

---

## What changed as a result

| | |
|---|---|
| `tests/test_hot_path.cpp` | New. Counts allocations on the event path; check 6 |
| `tests/plugins/dice_roller.cpp` | New fixture plugin; check 7 |
| `tests/test_determinism.cpp` | Three RNG stream tests; check 7 |
| `tests/test_feeds.cpp` | Generator-versus-committed-sample test; check 10 |
| `CMakePresets.json` | `full` preset description explains the extra test; check 9 |

Tests: 214 → 223. Green on `debug`, `release`, `ci` and `ci-debug`; 224 on
`full`, for the reason in check 9.

---

# The vacuous-test audit

Session F, Part 0. A second audit, with a narrower question than the first:
**which tests in this suite would pass while measuring nothing?**

## Why it was run

Two bugs in two sessions had the same shape, and in both the symptom was a green
suite:

- A fixture theory placed in a scanned directory broke two determinism tests **by
  existing**, because they counted what the loader found.
- An empty `data/cache/stooq` turned the identity calibration test from *skipping*
  into *measuring nothing and reporting green*: it built the membership history,
  measured against an empty cache, found every case unresolved, and made **zero
  assertions**. "0 verified, 10 coverage-limited", in a `WARN` nobody reads.

A skipped test announces itself. A vacuous pass does not. The second one is worse
and it is the one this audit is about.

## Method

A test is at risk when **every assertion it makes is inside a loop over a set it
did not choose**. If the set comes back empty the loop runs zero times, no
assertion is evaluated, and the test reports a pass.

`scripts/` does not carry the analyser — it was a one-off — but the rule it
applied is simple enough to re-apply by hand: for each test, count assertions
inside loops against assertions outside them, and look for a size or emptiness
assertion that is not itself inside the loop.

Its limits, stated so the numbers are read correctly: it cannot see a guard
expressed through a derived variable (`REQUIRE(loaded >= 2)`), nor one placed
inside an outer loop, so it over-reports. Every candidate it produced was read
before being classified. It has no false *negatives* of the kind that matter — a
test with an unguarded loop is always flagged.

## Counts

Scanned **369 C++ `TEST_CASE`s and 195 Python test functions**.

| | |
|---|---|
| Flagged: every assertion inside an unguarded loop | 23 |
| Flagged: some assertions inside an unguarded loop | 22 |
| Flagged: no assertions found at all | 1 |
| **Total candidates reviewed** | **46** |

After reading each one:

| | |
|---|---|
| **Genuinely vacuous** — would have passed with an empty set | **13** |
| **Partially vacuous** — some assertions skippable | **17** |
| **Not applicable** — loop bound is a literal, a compile-time constant, or a literal list | **15** |
| **No assertions at all** | **1** |

**31 tests were changed.** The 15 not-applicable cases are recorded here rather
than annotated in the source: a loop over `{Appearance::Dark, Appearance::Light}`
or `for (int i = 0; i < 500; ++i)` cannot iterate zero times, and a comment
saying so on each one would be noise.

## The two worth naming

**`a theory with one point is not drawn as a line`** asserted that every series
in the chart model had at least two points. That is trivially true of *no series
at all* — and no series at all is exactly what a bug in the filter it was testing
would produce. The test could not fail in the direction it was written to catch.
It now asserts the model holds exactly one series, and that it is the benchmark.

**`the fixture has no exposure ties`** made no assertions whatsoever. It called a
helper that raises on a tie and returns quietly otherwise — including when it
examined nothing, which an empty bar table or a rebalance interval longer than the
panel would both produce. The helper now returns the number of rebalance dates it
examined, refuses to return zero, and the test asserts the count.

## What changed, mechanically

Every test that discovers its inputs or is parameterized over a set now asserts
the set is **non-empty** and asserts its **expected size**, exactly rather than as
a floor. A floor cannot distinguish "somebody added a case" from "the file stopped
being read"; an exact count fails on both and makes updating it deliberate.

Two counts are now shared constants asserted from both sides:

| | |
|---|---|
| `kCommittedCases` / `EXPECTED_CASES` = 54 | Rows in `data/samples/sizing_cases.csv`, asserted by `tests/test_sizing.cpp` and `research/tests/test_sizing.py` against the same file. |
| `kExpectedTheories` = 2 | Theories the committed tree builds and loads. A floor of two passes when a third silently stops loading. |

One assertion caught an error while being written: `REQUIRE(history.decisions.size() == 2)`
in the identity test was wrong — the fixture produces one gap, not two — and it
failed immediately. That is the argument for exact counts in a single line.

## The negative control

`tests/negative_control.cpp` contains one deliberately false assertion. CTest runs
the binary and requires the reported failure to appear in its output, so the
suite's pass condition now includes **Catch2 having evaluated a false expression
and said so**.

`PASS_REGULAR_EXPRESSION` rather than `WILL_FAIL`, deliberately: `WILL_FAIL` only
checks that the process exited non-zero, which a crash on startup also does.
Matching the reported failure proves assertions specifically still work.

It is a separate binary, registered by hand rather than through
`catch_discover_tests`, because a deliberate failure inside a normal test binary
makes that binary exit non-zero — indistinguishable from a real regression to
everything downstream, including `scripts/check_skips.py`.

**Verified by inverting it.** With the assertion made true, the CTest entry fails,
because the expected failure text never appears. That check was run once and the
file restored; without it the control would itself be unfalsifiable, which is the
failure it exists to prevent.

## `ctest --output-junit` is useless for skip detection

Recorded so that nobody reaches for it later believing it works.

A run with 351 passes and one skipped test writes a JUnit file containing **351
`testcase` elements, every one `status="run"`**, and `skipped="0"` on the suite.
The skipped test is absent entirely. Anything trusting that file would conclude
the suite had no skips.

`scripts/check_skips.py` therefore parses the block CTest prints on the console:

```
The following tests did not run:
        122 - the classifier agrees with the cases we are sure of (Skipped)
```

Uglier, and correct. Ten tests cover the parser and the allowlist.

## What this does not establish

That every test is *good*. The audit asks one question — can this pass without
measuring anything — and answers it. A test that asserts something true but
uninteresting passes this audit and is still a weak test.

It also does not close the class. Two bugs of this shape were found by accident
before anyone went looking; a third will have a shape nobody has thought of yet.
What has changed is that the most common form of it now fails the build.

