# Cleanup audit

Eight sessions of accretion, inventoried before anything is touched. Written and
committed **before** the first change, so that what follows can be checked
against it rather than justified by it.

**The contract this session works under:** cleanup is behaviour-preserving, and
preservation is proven rather than assumed. Every golden value, parity
assertion, determinism hash and published number must be identical before and
after. A refactor that requires editing a test is not a refactor.

Baseline taken at `914dc20` (Session F2 Parts 3-5), CI green.

---

## 1. Baseline metrics

Counted from `git ls-files` only, so a stale build tree can neither flatter nor
inflate the numbers.

| | |
|---|---|
| tracked files | 315 |
| tracked bytes | 6,358,758 (6.06 MB) |
| C++ source (`.cpp`) | 120 files, 34,692 lines |
| C++ headers (`.hpp`) | 65 files, 11,292 lines |
| C headers (`.h`) | 2 files, 328 lines |
| Python | 50 files, 12,040 lines |
| Markdown | 22 files, 6,409 lines |
| CMake / text | 17 files, 1,076 lines |
| **total code + docs** | **79,789 lines** |

| | |
|---|---|
| C++ `TEST_CASE` blocks | 467 across 30 suites |
| tests ctest registers | 466 (`ci` and `ci-debug`) |
| permitted skips | 1, allowlisted with a written reason |
| Python test functions | 195 across 15 files |
| built binaries (`ci`) | 37, 20,697,600 bytes |
| full rebuild, `ci`, `-j 3` | 71 s |

The gap between 467 `TEST_CASE` blocks and 466 registered tests is
`negative_control.cpp`, which declares two cases and is hand-registered with a
single `PASS_REGULAR_EXPRESSION` entry rather than through
`catch_discover_tests`.

---

## 2. Untracked and ignored files

`git status --porcelain --untracked-files=all` is **empty**: the working tree has
no untracked file after a full build, a full test run and several `ms_live`
invocations.

Ignored directories present in the tree, all expected and all matched by
`.gitignore`:

```
.pytest_cache/   data/cache/   out/
research/__pycache__/   research/tests/__pycache__/   scripts/__pycache__/
```

No recording (`*.ndjson`) is present in the tree. The live sessions write to
`%TEMP%`, and `ms_live equivalence` deletes its recording unless `--keep`.

**Classification: nothing to do.**

---

## 3. Tracked files nothing references

**Method.** For every tracked text file, search every *other* tracked text file
for its basename, and for a header also for its include path. Zero external
mentions makes a file a candidate for this list -- not a candidate for deletion,
because several things here are deliberately unreferenced.

**One correction to that method, recorded because it produced a false finding.**
The first pass reported `include/marketscan/abi/export.h` as included by nobody.
It is included by `plugin_abi.h` line 49 as `#include "export.h"` -- a *relative*
include, which a search for the full path cannot see. There are no unreferenced
headers in this repository.

| file | why it has no references | class |
|---|---|---|
| `.gitattributes` | git reads it; nothing cites it | protected |
| `assets/fonts/LICENSE-Inter.txt`, `LICENSE-JetBrainsMono.txt` | licence texts for vendored fonts; required by those licences | protected |
| `pyproject.toml` | discovered by Python tooling | protected |
| `research/__init__.py`, `research/tests/__init__.py` | package markers | protected |
| `research/tests/test_*.py` (11 files) | discovered by pytest by name convention | protected |
| `scripts/make_daily_sample.py` | generated a committed fixture; kept so the fixture can be regenerated | protected |
| `scripts/make_quickstart_notebook.py` | generated the committed notebook, same reason | protected |
| `scripts/format.ps1` | developer convenience; CI invokes `clang-format` directly | **needs a decision** (see §3.1) |

### 3.1 `scripts/format.ps1`

Not referenced by CI, by `README.md`, or by any doc. It works, it is what a
developer should run before committing, and nothing tells them it exists.

This is a documentation gap rather than dead weight. Deleting it would remove
the only convenient way to format the tree; documenting it is a docs change.
**Needs a decision**, not touched this session.

### 3.2 Files with exactly one point of reference

Not a problem, recorded so that a future sweep does not mistake a single
reference for none. `tests/*.cpp` are each named once, by `tests/CMakeLists.txt`.
`research/*.py` modules are each named once, by `research/README.md`.
`src/version.hpp.in` is named once, by `src/CMakeLists.txt`.

---

## 4. Dead code

### 4.1 Commented-out code: none

Scanned every tracked `.cpp`, `.hpp` and `.h` for runs of three or more
consecutive comment lines that look like statements. Two hits, both arithmetic
derivations in `tests/test_metrics.cpp` explaining an expected Sharpe and
Sortino by hand. **No commented-out code in the repository.**

### 4.2 TODO / FIXME / HACK / XXX: none

Three hits, all inside PNG binaries where the byte sequence `XXX` occurs by
chance. **No outstanding markers in any source, script, spec or document.**

### 4.3 Public names nothing in this tree calls

**Method.** For each name declared in a public header, count tracked files
mentioning it. The nine below are mentioned only by their own declaration.

| name | header |
|---|---|
| `has_quote` | `book/order_book.hpp` |
| `sink_count`, `source_count` | `core/engine.hpp` |
| `first_seen`, `last_seen`, `primary_ticker` | `data/security_master.hpp` |
| `param_enum` | `strategy/istrategy.hpp` |
| `set_session_zone` | `strategy/istrategy.hpp` |
| `shadow_root` | `strategy/plugin_loader.hpp` |

**All nine are classified "needs a decision", none as deletable**, for two
reasons. They are public API on documented classes, so removing them is a
behaviour change for anything outside this tree -- and `param_enum` exists
specifically for plugin authors, who are outside this tree by construction.
And the GUI is built with `MARKETSCAN_BUILD_GUI=OFF` in CI, so "CI is green
without it" proves less here than it would elsewhere; the search covered the
tracked GUI sources, but a consumer that is not in the tree is not covered by
any search.

---

## 5. Duplicated logic within one layer

Genuine copy-paste, all of it introduced during Sessions F and F2, all of it
inside the live layer. **The deliberate cross-layer duplication between the C++
engine and the Python research layer is not in this list and is not touched --
see §7.1.**

**Every pair was extracted and compared before being listed here.** Comments and
whitespace normalised away, all copies of all eight helpers are identical. Had
any differed, collapsing them would have been a behaviour change wearing a
refactor's clothes.

| helper | copies | where | class |
|---|---|---|---|
| `number(double, int)` | 2 | `live_broker.cpp`, `order_tracker.cpp` | safe to refactor |
| `local_now()` | 2 | `live_broker.cpp`, `live_feed.cpp` | safe to refactor |
| `interruptible_sleep()` | 2 | `live_broker.cpp`, `live_feed.cpp` | safe to refactor |
| `tif_name()` | 2 in `src/live` | `live_broker.cpp`, `recorded_broker.cpp` | safe to refactor |
| `option_value()` | 4 | `ms_live.cpp`, `equivalence_cmd.cpp`, `dryrun_cmd.cpp`, `orders_cmd.cpp` | safe to refactor |
| `has_flag()` | 2 | `equivalence_cmd.cpp`, `positions_cmd.cpp` | safe to refactor |
| `split_commas()` | 2 | `ms_live.cpp`, `equivalence_cmd.cpp` | safe to refactor |
| `tif_name()`, `type_name()` third copy | 1 each | `apps/live/equivalence.cpp` | **needs a decision** |

The last row is separated deliberately. Collapsing a formatting helper shared
between `src/marketscan/live/` and `apps/live/` means deciding that the library
owns display formatting, which is a design choice rather than a refactor. Left
alone.

**A convention constraint on the two refactors.** There is no precedent in this
repository for a header under `src/`: every header lives in
`include/marketscan/`. Introducing a `src`-local header to hold shared helpers
would itself be convention drift, so the live-layer helpers go into
`include/marketscan/live/`, and the CLI helpers into `apps/live/`, which already
carries `equivalence.hpp`.

---

## 6. Files over 800 lines

Excluding binaries (fonts, screenshots, figure JSON).

| lines | file | what it is | class |
|---|---|---|---|
| 1234 | `include/marketscan/strategy/istrategy.hpp` | the plugin-facing shim: base class, parameter binding, pending-order bookkeeping, session clock, and the export macro | **needs a decision** |
| 1065 | `docs/STATUS.md` | the handoff document | protected |
| 1038 | `tests/test_universe.cpp` | 23 cases on membership, identity and forced exits | leave |
| 1025 | `tests/test_parity.cpp` | 15 cases, the bit-exact cross-layer comparison | protected |
| 960 | `apps/marketscan/session.cpp` | the GUI's engine-owning worker thread | **needs a decision** |
| 941 | `docs/EXECUTION.md` | execution realism and the live seam | protected |
| 918 | `tests/test_live_broker.cpp` | 37 cases on the order lifecycle | leave |
| 889 | `src/marketscan/strategy/plugin_loader.cpp` | discovery, shadow-copying, ABI checking, instantiation | **needs a decision** |
| 874 | `src/marketscan/data/universe.cpp` | membership history and point-in-time queries | **needs a decision** |
| 869 | `research/parity.py` | the Python half of the parity pair | protected |

`istrategy.hpp` is the strongest candidate for a split along an obvious seam --
the export macro and the shim functions are separable from the base class. It is
also the single most dangerous file in the repository to touch: every plugin
compiles it, and the v1 reference plugin's byte-identical behaviour is asserted.
**Not split this session.** It is recorded as the largest available refactor and
the one most deserving of its own session with the v1 plugin comparison in front
of it.

The four test files are left alone: length in a test file is coverage, and
splitting them buys nothing but churn.

---

## 7. Spec and code drift

Ten documents in `docs/` plus five in `docs/specs/`. Comparing normative
statements against the implementation.

**Two corrections to this session's own brief, recorded because the brief's
assumptions are part of the drift.**

- The brief says "every spec file in the repo root". There are no spec files in
  the repo root; the only root Markdown is `README.md`. The specs are in
  `docs/specs/`.
- The brief cites `SESSION_D.md` Part 1 and `SESSION_F.md` Parts 3-5 as
  prerequisite reading. **Neither file exists**, and no `SESSION_*.md` was ever
  committed. Every session prompt in this project was pasted into a
  conversation. The consequence is §7.1.

### 7.1 The most important finding in this audit

The brief's strongest prohibition is *do not deduplicate the C++ engine and the
Python research layer; the duplication is deliberate,* and it cites
`BUILD_SPEC_2.md` §2 and `SESSION_D.md` Part 1 as saying so.

**Neither says so. Nowhere in this repository says so.**

What the repository contains is the *result* -- `STATUS.md` line 130, "Two
independent implementations of one strategy agree to the micro-dollar", and
`README.md` line 27 in the same spirit. A reader encountering those, and no
statement that the duplication is load-bearing, could reasonably conclude that
two implementations which agree exactly present an opportunity to remove one.

That is the single most expensive mistake available in this codebase, and it is
the one thing a cleanup pass is most likely to attempt. The rationale exists
only in conversation history.

**Class: needs a decision** -- writing it down is a docs change and belongs in
this session's documentation part, not in a refactor commit.

### 7.2 `BUILD_SPEC_2.md` §160 still prescribes a Stooq fetcher

> "A **Stooq** fetcher (Python, under `scripts/`): free daily OHLCV, no API key."

`DATA_POLICY.md` records that Stooq put a proof-of-work browser challenge in
front of that endpoint, that solving it was declined, and that the fetch has
never succeeded. `STATUS.md` says `scripts/fetch_stooq.py` is kept because it
reports the gate correctly, not because it works.

The spec asserts an available capability the project has documented as
unavailable. **The spec was superseded and should carry a note saying when and
by what** -- recorded, not fixed.

### 7.3 `on_session_close` ordering

The brief attributes this to `BUILD_SPEC.md`'s M1 text. `on_session_close` does
not appear in `BUILD_SPEC.md` under any spelling. It is specified in
`BUILD_SPEC_2.md` lines 137 and 149, as firing "**before** delivering the first
event" of the next session, once per session.

`SIZING.md` line 55 and `EXECUTION.md` line 304 both describe the same ordering
in more operational terms. **No drift found between them.** The brief's
attribution is wrong; the specification itself is consistent with the code.

### 7.4 M3b is a milestone the specs never defined

`M3b` appears five times in `STATUS.md` and zero times in `docs/specs/`. The
Session E reclassification -- from "blocked on Stooq" to "CRSP-dependent" -- has
therefore nothing in the specs to overtake. The milestone exists only in the
handoff document. **Recorded; no action.**

---

## 8. The vacuous-pass law: where it is not yet applied

The law itself, and its four instances, belong in `docs/AUDIT.md` and are written
up there. This section is the backlog: places the law is not yet applied.
**Applying any of it is behaviour change and is forbidden this session.**

### 8.1 Method, and a false finding it produced first

A first scan flagged 14 test cases. It was wrong about most of them: its regex
could not cross nested parentheses, so
`REQUIRE(ledger.blotter().size() == 20'000)` read as an absent size assertion.
An audit containing false claims is worse than no audit, so the scan was
rewritten to track brace depth and classify each assertion as inside or outside
a loop.

The corrected scan flags a case only when it iterates a collection whose length
it did not itself write down **and** no assertion outside that loop constrains
that length.

### 8.2 Result: one genuine gap in 467 cases

| file | case | gap |
|---|---|---|
| `tests/test_stream_parser.cpp:255` | "the gate leaves an already ordered stream untouched" | Parses three trades from a literal and loops over `parsed.events`. If the parser returned zero events the loop would iterate nothing and all four assertions outside it would still pass. Wants `REQUIRE(parsed.events.size() == 3)`. |

The second case the corrected scan flagged,
`tests/test_determinism.cpp:741`, is a false positive: it constrains the set
with `REQUIRE(sorted_ids(ids) == kExpectedTheories)`, a comparison against a
two-element literal, which pins the length exactly. The scan looks for `.size()`
and `.empty()` and does not recognise that form.

**One gap in 467 cases** is the honest result, and it says the law has largely
been applied already -- which is what four sessions of applying it by hand
should look like.

### 8.3 Summaries whose arithmetic is not asserted to reconcile

`ms_live equivalence` and `ms_live record` print tallies -- submitted, accepted,
filled, partial, cancelled, rejected -- and **nothing asserts that they add up.**

This is not hypothetical. On 2026-09-23 a run printed "128 submitted, 124
accepted, 0 rejected" while four orders had plainly been refused and logged; the
refactor that extracted `OrderTracker` had dropped that counter. Nothing was
lost, the orders were all in the log with their reasons, and the only reason it
was caught is that a person read the line and the arithmetic did not work.

That is the law's corollary in its purest form: **attention does not run in CI.**

**Backlog, not fixed:** a reconciliation assertion on the report -- submitted =
accepted + refused-by-rail + refused-by-venue + unknown -- would have failed the
run instead of printing a wrong number. Adding it is behaviour change.

---

## 9. What this session will and will not do

### Acting on

| entry | action |
|---|---|
| §5, live-layer helpers (4 duplications) | one refactor commit, one shared header under `include/marketscan/live/` |
| §5, CLI helpers (3 duplications) | one refactor commit, one shared header under `apps/live/` |
| §8, the law and its four instances | written into `docs/AUDIT.md` |
| §7.1, the deliberate-duplication rationale | written down where a cleanup pass will read it |
| §7.2, the Stooq spec | a superseded-by note, no change to the claim's history |
| repository hygiene | §10 |

### Not acting on

Everything classified **needs a decision**: `scripts/format.ps1`'s documentation
gap; the nine uncalled public names; the third copies of `tif_name` and
`type_name` spanning library and app; splitting `istrategy.hpp`,
`session.cpp`, `plugin_loader.cpp` or `universe.cpp`; and the vacuous-law
backlog in §8.2 and §8.3.

**Nothing is deleted.** Every candidate that a mechanical sweep would have
deleted turned out to be either protected or load-bearing, and the one file
with no home -- `scripts/format.ps1` -- needs documenting rather than removing.
This section exists to say that plainly: the deletion commit this session
planned for has no contents.

---

## 10. Repository hygiene

Checked, with results in §11 after the work is done.

- `.gitignore` completeness against build trees, caches, credential paths,
  editor state, the data cache and recording output.
- Tree and history scanned for anything resembling a credential.
- No market data and no session recording tracked.
- Line endings and `.gitattributes`.
- `LICENSE` present; vendored font licences present and compatible.
- No tracked build artifact or file over a few megabytes.

---

## 11. Close

Completed at the end of the session: after-metrics beside before-metrics, the
statement that every golden value and determinism hash is unchanged and how that
was verified, and the backlog this audit produced.
