# Protected: things that look like cruft and are load-bearing

**If you are here because you are cleaning something up: read the row before you
delete the thing.**

Every entry below has survived at least one session that could reasonably have
removed it. Each one looks, from the outside, like a leftover — a stale fixture, a
test that fails on purpose, a duplicated check, an unreachable assertion, a script
that does not work. In every case the appearance is the point, and removing it
destroys a measurement while leaving every number in the repository intact.

That last clause is why this file exists. These are not things whose deletion
breaks the build. They are things whose deletion makes the build pass while
proving less, which is the failure mode this project has hit five times and
documented in [`AUDIT.md`](AUDIT.md) as the vacuous-pass law.

**This list came from Session G's brief, which is not in the repository** — see
[`briefs/README.md`](briefs/README.md) for why that kept happening and what now
prevents it. It lived in a prompt for two sessions, which is exactly as durable as
it sounds.

---

## 1. Fixtures whose value is that they are wrong

| what | why it must stay |
|---|---|
| `tests/negative_control.cpp` | **One assertion in it is false on purpose.** "All green" is information only if red is reachable; a suite where every test passes and a suite whose harness stopped evaluating assertions look identical from outside. CTest requires this binary's output to *contain* its failure, so the pass condition for the whole suite includes Catch2 having evaluated a false expression and reported it. It is a separate binary precisely so its deliberate failure is not confused with a regression. Its own header explains the two times this project shipped a check that had stopped checking. |
| `tests/plugins/v1_reference.cpp` | **Its value is that it is stale.** It declares ABI v1 by hand after the host moved to v2, filling exactly the nine slots v1 had and ignoring the tenth — which is what a DLL compiled against the old header does. It proves the migration promise: a v1 plugin keeps loading, behaves byte-identically, and never receives the newer callback. It is source rather than a committed binary so it can be rebuilt and audited. |
| `tests/plugins/reference_body.hpp` | Shared by the v1 and v2 reference plugins so the two **cannot differ in anything but their declared version**. Inlining it into either would make the byte-identical claim untestable. |
| `tests/plugins/dice_roller.cpp` | The only thing in the tree that draws from the host's per-strategy RNG. Neither shipped theory does, so through all of Spec 1 that machinery was plumbed, documented and never once run — see [`AUDIT.md`](AUDIT.md) check 7. Delete this and the RNG goes back to being untested. |
| `tests/plugins/bad_abi.cpp`, `null_create.cpp`, `empty_id.cpp`, `no_exports.cpp` | Each is a specific way a plugin can be broken. They live outside the scanned directory so the app never loads them. A loader tested only against working plugins is a loader whose error paths have never run. |
| `tests/allowed_skips.txt` | The allowlist `scripts/check_skips.py` reads. Deleting it does not relax the gate — it makes *every* skip unexpected, which is the safe direction — but it also deletes the written reason the one permitted skip is permitted. |

## 2. Guarantees paired with checks that cannot fire

This is the category most likely to be "simplified", because the check is provably
unreachable and deleting unreachable code is normally correct.

| the guarantee | the check | why both |
|---|---|---|
| `RecordedBroker::submit` registers each order with **the id the recording says it had** (`register_order_with_id`, `recorded_broker.cpp:191`), so the two sides cannot number differently because only one side numbers | a venue response matching no live order is fatal, surfaced as `MISROUTED` in `equivalence_cmd.cpp:640` | The guarantee makes the bug impossible. It is one refactor, one new code path, or one caller doing its own numbering away from being untrue. On that day the check is the difference between a stopped run and another false `EQUIVALENT` — which is what this pair cost a live market session to find. |

**Apply the law to the check itself:** its output is identical whether it is
working or absent, which is precisely the argument for keeping it, not for
deleting it. The full account is in [`AUDIT.md`](AUDIT.md) under "The response: a
guarantee and a check, deliberately not one mechanism".

`RecordedBroker` as a whole is protected for a related reason: it exists so that a
replay compares live against replay rather than comparing two implementations of
the order state machine. Folding its logic into the fill simulator would recreate
the divergence it was written to eliminate.

## 3. Deliberate duplication across the two layers

`theories/decile_parity/` and `research/parity.py`; `include/marketscan/strategy/sizing.hpp`
and `research/sizing.py`.

Two implementations agreeing is evidence **only while they are causally
independent**. Delete either half and the assertion still passes having compared
nothing; give them a shared core and they agree because they are the same code.
Every printed number survives both moves.

The full argument, what is forbidden, and what the mechanical guard does and does
not catch are in [`AUDIT.md`](AUDIT.md) under "Deliberate duplication".
`research/tests/test_layer_independence.py` now enforces the mechanical part.

**Duplication *within* one layer is ordinary copy-paste and stays collapsible** —
Session G collapsed eight such helpers. The rule is about the seam between the
layers, not a licence for repetition on either side of it.

## 4. Guards whose passing output looks like no guard at all

| what | why it must stay |
|---|---|
| The `importlib` / `__import__` ban in `research/tests/test_layer_independence.py` | Stricter than strictly necessary, deliberately. A module name resolved at runtime is invisible to an import-graph walk, so this is the one-line bypass; leaving it open would make the guard decorative. |
| `SummaryAudit::ok()` returning **false when nothing was checked** | An audit with no identities in it prints what an audit of everything prints. Making zero checks a pass would reintroduce the exact shape the class exists to close. |
| `REQUIRE(parsed.events.size() == 3)` in `tests/test_stream_parser.cpp`, and the companion case "an unused gate reports the same zeros as a gate that found nothing wrong" | The assertion looks redundant. The companion case exists to prove it is not: an unhandled message kind yields zero events, and against that input all four gate counters read exactly what they read after three well-ordered events. Remove either and the case can pass having measured nothing. |
| `ms_live equivalence`'s `VACUOUS` exit 3 | Zero decisions compared is not a pass. Three runs reported success on empty comparisons before this existed. |
| The `--min-tests` floor in `scripts/check_skips.py` | **A catastrophic-failure guard, not a drift detector.** See §4.1 — the distinction is the whole reason the number looks wrong and is not. |

### 4.1 The `--min-tests` floor is 400 against a suite of 483, and that is correct

It looks stale. It is not, and the itch to "fix" it comes from reading it as the
wrong kind of mechanism.

**What it is:** a catastrophic-failure guard. It answers *did the suite run at
all* — the wrong `--test-dir`, a preset whose test set collapsed, a `-R` filter
matching a handful. It does that job identically at 483 tests or at 600, because
every one of those failures lands far below 400.

**What it is not:** a drift detector. It cannot tell you that eighty tests
silently disappeared, and tracking the count would make it worse rather than
better — a number anyone bumps on every addition is a number nobody reads, and a
guard nobody reads is a guard that gets bumped past a real regression without
anyone pausing.

**If drift detection is wanted**, it is a different mechanism with a different
shape: a ratchet against a recorded count, where any decrease is an explicit
reviewed act. That is worth deciding deliberately rather than arriving at by
gradually tightening a floor into a role it was never built for. Note that the
test manifests in §4.2 already cover most of what a ratchet would: a disappeared
test fails by name, which is strictly more informative than a count going down.

**So: do not raise it to track the suite, and do not lower it to zero.** If you
want the other mechanism, build the other mechanism.

### 4.2 The test manifests, and what they now protect mechanically

`tests/test_manifest.txt` and `research/tests/test_manifest.txt` record every
registered test by name, checked by `scripts/check_test_manifest.py` in CI.

**This is what turned most of §1 and §4 from guarded-by-reading into guarded.**
Deleting any named test now fails until the name is removed from the manifest in
the same commit — a diff a reviewer sees. It protects every guard that exists and
every guard added later, without anyone remembering to write bespoke protection for
each one.

Two manifests rather than one, because the suites are listed by different tools and
no CI job has both a build tree and pytest's dependencies. That split matters more
than it sounds: the guard standing between this project's central architectural
claim and its becoming tautological is `test_no_guarded_module_imports_dynamically`,
and it is a **pytest** test. A manifest covering only C++ would have protected
everything except the thing most worth protecting — which is what the first draft
did, and what its own test caught.

An unrecorded *new* test also fails. That is deliberate: it costs one `--write` per
commit that adds a test, and it buys the property that a guard written today is
protected tomorrow by default.

### 4.3 The documented claims, checked against reality

`scripts/check_doc_claims.py` validates the load-bearing factual assertions in the
docs: the test counts, the ABI version, that the guards `README.md` names still
exist and are still wired into CI, and that no test reaches the network.

It exists because `README.md` said **"no network code, no API clients, no
credentials"** until Session F made it false, and nobody noticed for three
sessions. The stale test counts in `STATUS.md` are the same failure in a cheaper
costume, and one step covers the class. On its first real run it caught a count
that was thirteen low.

Each claim is **anchored to its sentence rather than grepped**, because
[`FREEZE_CONTRACT.md`](FREEZE_CONTRACT.md) forbids correcting historical
statements and four documents correctly say the ABI is "frozen at v1". A checker
that flagged those would deserve to be switched off. A pattern that stops matching
is a failure, not a skip.

## 5. Tools kept for what they report, not what they do

| what | why it must stay |
|---|---|
| `scripts/fetch_stooq.py` | **It does not work and is kept anyway.** Stooq put a proof-of-work browser challenge in front of the endpoint and solving it was declined. The script is kept because it reports that gate correctly: deleting it would turn a documented refusal into an absence, and the next person would rediscover the wall. See [`DATA_POLICY.md`](DATA_POLICY.md). |
| `ms_live dryrun` | Runs a plugin against a recording with no network. A live session needs an open market, uses the account's one stream connection, sends real orders and cannot be repeated. This is what stops one being spent discovering a parameter was wrong — which it has already done, catching a configuration demanding $471k against $400k of buying power. |
| `ms_live bait` | Sends each rejection candidate to the venue alone and reports what came back. Two rejection mechanisms had been relied on without ever being seen to refuse anything; this is what ended that. Its value is the measurement, not the code. |
| `theories/lifecycle_exercise/` | Loses money by design and says so in its header. It exists to drive orders through every state a venue can put them in. Judging it as a strategy is a category error. |
| `theories/_template/` | The starting point a plugin author copies. Nothing in the tree *references* it, by design — but it is **not** unguarded: `theories/CMakeLists.txt` builds it, so an ABI change that broke the copy-to-start skeleton fails the build, and `tests/test_session.cpp` loads the resulting DLL to prove the drop-in-a-folder claim, `SKIP`ping if it is absent — which fails the unallowlisted-skip gate. Deleting it therefore breaks two things loudly. |

## 6. Data and decisions that cannot be regenerated

| what | why it must stay |
|---|---|
| `data/security_overrides.csv` | Hand-written company facts, not measurements. Four entries where a threshold decided wrongly and a person overruled it. Regenerating it is not possible; the information is not in any price series. |
| `docs/IDENTITY_DECISIONS.md` | The record of which identity calls were made by measurement and which by judgement. Without it the overrides above are four unexplained rows. |
| `data/samples/` | Every committed fixture. The parity panels, the sizing cases, the daily sample. Golden values are asserted against these bytes; a regenerated fixture is a different measurement wearing the same filename. |
| `docs/specs/` | Records **what was asked for** — not what was built, and not what turned out to be possible. Never corrected, even when superseded; a superseded requirement gets a dated note beneath unmodified text. See [`FREEZE_CONTRACT.md`](FREEZE_CONTRACT.md). |
| `docs/figures/`, `docs/screenshots/` | Outputs of runs that were measured once. They are evidence, not decoration. |

---

---

## What is guarded by what

Stated plainly, because a protected list that does not say which of its rows a
machine enforces is a list whose reader cannot tell reading from checking.

| | |
|---|---|
| every named test in §1, §4 | the manifests — deleting one fails CI by name |
| `theories/_template/` | built in CI; a test loads the DLL and skips if it is gone |
| the ABI version, the test counts, the guards `README.md` names, no-test-network | `check_doc_claims.py` |
| the live-money hostname, tracked credentials | `check_no_live_endpoints.py`, `check_no_secrets.py` |
| the cross-layer duplication, mechanically | `test_layer_independence.py` — direct, transitive, deletion, dynamic-import evasion |
| the cross-layer duplication, against transliteration | **nothing.** A review question: was this derived from `SIZING.md`, or from the other implementation? |
| `scripts/fetch_stooq.py`, `ms_live bait`, `ms_live dryrun` | **reading this file.** Low stakes: each is recoverable from git history, none is load-bearing for a measured result, and an honest "guarded by reading" is an adequate answer for them. |
| `data/security_overrides.csv`, `docs/IDENTITY_DECISIONS.md`, `data/samples/` | reading this file, plus the tests that assert golden values against the fixtures |

The rows in the last two groups are the honest remainder. They are named here
rather than left implicit, so that "this list is partly mechanical" is a fact a
reader has rather than a discovery they make.

## How to add to this list

An entry belongs here when **removing the thing would leave the build green and
the repository measuring less.** That is a narrower test than "this is
important": a load-bearing file whose deletion breaks compilation does not need
protecting, because the compiler protects it.

Write the reason beside the entry. A list of paths with no reasons is a list
someone will override the first time one of them is inconvenient.

## What this list is not

It is not a freeze. Any entry here can be removed by a deliberate decision that
engages with its reason — and this file exists to make sure that decision is
deliberate rather than a side effect of tidying up. If the reason no longer holds,
say so, and delete the row with the thing.
