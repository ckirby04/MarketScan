# MarketScan — Addendum: Research View

**Applies to:** the GUI (`apps/marketscan`), after `SPEC_VIEW_MODES.md` (M9b).
**Prerequisite reading:** `SPEC_VIEW_MODES.md` in full, `docs/DATA.md`, `docs/DATA_POLICY.md`,
`docs/IDENTITY.md`, `docs/FALSE_DISCOVERY.md`, and the four contracts this view is built on:

| contract | files, under `contracts/research/` |
|---|---|
| Strategy Form | `strategy_form.schema.json`, `FORM.md`, `examples/` |
| Data catalog | `CATALOG.md`, `catalog_records.schema.json` |
| Results | `RESULTS.md`, `results.schema.json` |
| Assistant | `assistant/system_prompt.md`, `assistant/tools.json` |

They live in `contracts/` rather than here because code loads them and they change with the
system, and the documents in `docs/specs/` are never edited.

---

## 1. What this is

A third view mode, **Research**, where a question asked in plain English becomes a backtest:

> "I wonder what the optimal drawdown is to buy into TQQQ over the past 20 years."

The assistant turns the question into a Strategy Form, asks only what it cannot infer, and
shows the form. The user runs it. The engine runs the sweep, grades it against fixed gates,
and returns a Results document. The view shows the verdict, then the gates, then the stats,
and the assistant's analysis last.

Overview asks *is this working?* Operator asks *what is it doing?* Research asks *is this idea
any good, according to history?* It is a different question again, which is why it is a mode
and not a panel.

---

## 2. What I do NOT want

All prior NOT-lists remain in force. Additions:

- Do **NOT** let the view compute a number. Every figure in the Research view is read from the
  Results document, the same way Overview and Operator read the snapshot. If the view needs a
  number that isn't in the Results, it goes into the Results schema.
- Do **NOT** let the assistant produce the verdict, the gates, or any statistic. It fills forms
  and writes analysis. The grade comes from the engine by published rules.
- Do **NOT** give the assistant, or anything in the Research path, a route to a broker. No
  order tool, no positions tool, no link against `live/`. The research path can be wrong; it
  must not be able to be wrong with money.
- Do **NOT** add the model's host to the broker allowlist. It goes on a separate list, used by
  a separate client (Part R2).
- Do **NOT** show "expected return", "recommended", "buy", or "good idea" anywhere. The grade
  reads "Historical evidence: supported", and the outcome range is labelled as resampled
  history, not a forecast.
- Do **NOT** block the frame on the network or on a backtest. Same rule as `Session`: the GUI
  posts commands and reads snapshots.
- Do **NOT** make the assistant a requirement. With no API key the view still works, and a
  form can be filled in by hand.

---

## 3. The non-negotiable rule

> **One Results document. One renderer. The assistant writes words, never numbers on its own.**

This is the snapshot rule from `SPEC_VIEW_MODES.md` §2 extended to a new source. The Results
document is to the Research view what `SessionSnapshot` is to the other two: complete,
immutable once published, and the only thing the view draws from.

The assistant's analysis is shown below the stats and labelled as written by AI from the
numbers above. If the analysis and the stats ever disagree, the stats are right by
construction, and that is the reason for the order.

---

## 4. The view

Three states, one window. The conversation rail stays on the left in the last two so a
follow-up is always one line away.

### 4.1 Ask

Shown when there is no conversation.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                                                              │
│              Ask a question about a trading rule                             │
│   ┌──────────────────────────────────────────────────────────────────────┐   │
│   │ i wonder what the optimal drawdown is to buy into TQQQ over the ...  │   │
│   └──────────────────────────────────────────────────────────────────────┘   │
│                                                   Ctrl+Enter to ask          │
│                                                                              │
│   Try:  Buy SPY after a 20% drop, sell at a new high                         │
│         Does a 50/200 day crossover beat holding QQQ since 2005?             │
│                                                                              │
│   Recent runs                                    grade           data        │
│   TQQQ drawdown entry, 2006-2026          insufficient evidence  unverified  │
│   SPY 52-week-high dip, 2000-2026         fragile                unverified  │
│                                                                              │
│   [ Fill in a form by hand ]                                                 │
└──────────────────────────────────────────────────────────────────────────────┘
```

Recent runs are read from `runs/` (Part R5). Opening one restores its form, results and
conversation without re-running anything.

### 4.2 Form

The assistant has drafted a form and may be asking questions.

```
┌─ Conversation ───────────────┬─ Strategy Form ─────────────── Drawdown entry ─┐
│ You: i wonder what the       │ ASSET                                          │
│ optimal drawdown is to buy   │  TQQQ                                  stated  │
│ into TQQQ over the past 20   │  ⚠ Trading began 2010-02-09. 2006-2010 is      │
│ years                        │    simulated 3x QQQ (correlation 0.99x).       │
│                              │                                        default │
│ Assistant: TQQQ only has     │ DATES   2006-10-20 → 2026-10-20         stated │
│ data from 2010, so I've ...  │ ENTRY                                          │
│                              │  Drawdown from   [ ? choose ]         MISSING  │
│ ┌ Drawdown measured from? ─┐ │  Threshold       10% → 80%, step 5   inferred  │
│ │ ( All-time high        ) │ │  Buy             all at once          default  │
│ │ ( 52-week high         ) │ │ EXIT                                           │
│ │ ( High since last sale ) │ │  Sell when       [ ? choose ]         MISSING  │
│ └──────────────────────────┘ │ OPTIMIZE FOR     [ ? choose ]         MISSING  │
│ ┌ When should it sell? ────┐ │ ▸ Costs, cash, benchmarks, robustness (6)      │
│ │ ...                      │ │                                                │
│ └──────────────────────────┘ │ 15 variants · about 20 s      [ Run ] disabled │
│ [ follow-up...             ] │                                                │
└──────────────────────────────┴────────────────────────────────────────────────┘
```

- Every field shows its provenance as a word, not only a colour: `stated`, `inferred`,
  `default`, `MISSING`. Hover shows the `quote` or `note`.
- Warnings from provenance notes are drawn in the field, with a glyph and the status palette.
- Fields marked `x-ask: "never"` are collapsed under one row, editable but out of the way.
- Answering a question card writes the value into the form and marks it `stated`. Editing a
  field by hand does the same. Either one is sent to the assistant as context on the next turn.
- **Run** is enabled only when the form validates and nothing is `MISSING`. Beside it: the
  number of sweep points, and a time estimate from the engine (not the assistant).
- The form is drawn from the schema, one widget per type: enum → combo, `param` → a
  value/sweep/list switch with its inputs, date → date field, `oneOf` with a `type` const →
  combo that swaps the sub-form. No widget is written for a specific field. Adding a template
  must not need GUI code.

### 4.3 Results

```
┌─ Conversation ───────────────┬────────────────────────────────────────────────┐
│ ...                          │ HISTORICAL EVIDENCE                            │
│                              │ ◆ INSUFFICIENT EVIDENCE           ⚠ Unverified │
│                              │   OOS CAGR  +x.x% vs benchmark +y.y%           │
│                              │   OOS max drawdown  -a% vs -b%                 │
│                              │   Resampled histories  p5 · p50 · p95          │
│                              │   ⚠ MOSTLY_SYNTHETIC  ⚠ UNCALIBRATED_CORRECTION│
│                              ├────────────────────────────────────────────────┤
│                              │ WHY                                            │
│                              │  FAIL evidence         4 entries (needs ≥ 5)   │
│                              │  ──   oos_benchmark    not reached             │
│                              │  ...                                           │
│                              ├────────────────────────────────────────────────┤
│                              │ STATS   [Equity][Sweep][Timing][Range]         │
│                              │         [Regimes][Trades][Data]                │
│                              │  <equity chart + drawdown strip>               │
│                              ├────────────────────────────────────────────────┤
│                              │ ANALYSIS · written by AI from the numbers above│
│                              │  ...                                           │
│ [ follow-up...             ] │ [Edit form] [Open in Operator] [Export]        │
└──────────────────────────────┴────────────────────────────────────────────────┘
```

Top to bottom, never reordered:

1. **Verdict.** The grade as words in caps with a glyph, coloured from the status palette as
   reinforcement only (`supported` good, `fragile` warning, `not_supported` serious,
   `insufficient_evidence` muted ink, not a status colour, because "not enough data" is not
   bad news about the idea). The data grade beside it. Headline numbers. Warnings.
2. **Why.** One row per gate, in grading order: `PASS` / `FAIL` / `──` in the mono face, the
   gate name, and the engine's `summary` string. Gates after the deciding one are shown, not
   hidden, so the user sees everything that was measured.
3. **Stats.** Tabs:
   - *Equity*: the one `EquityChart`, rule vs benchmarks, with the drawdown strip, and the
     out-of-sample split drawn as a vertical rule. Synthetic history is shaded.
   - *Sweep*: one swept parameter is a line of the in-sample metric against the value,
     with out-of-sample as a second line on the same axis, the selected point marked and its
     plateau neighbours bracketed. Two swept parameters is a heatmap, with a sequential scale and
     the selected cell outlined. Excluded points are hollow, not omitted. More than two dimensions:
     a table, sortable.
   - *Timing*: histogram of the random-timing null with the rule's value as a marked line and its
     percentile written beside it.
   - *Range*: the bootstrap percentiles for rule and benchmark as two interval strips on one axis.
   - *Regimes*, *Trades*, *Data*: tables, using the existing `format_*` helpers and `money_cell`
     conventions, so a loss looks the same here as in the blotter.
4. **Analysis.** Streams in as it arrives. Labelled as AI-written, every time.

Buttons:

- **Edit form** goes back to 4.2 with this form loaded.
- **Open in Operator** replays the selected parameters as a normal `Session` and switches to
  Operator, so the blotter, positions, drawdown and log are all available for the one run that
  matters. This is the bridge between the modes: Research finds the run, Operator explains it.
- **Export** writes the run directory (Part R5) somewhere the user chooses.

### 4.4 While it runs

The results area shows stages with counts, all from the job's progress in the snapshot:
`sweep 37/150 · random timing · bootstrap · grading`. **Cancel** stops the job and discards
partial results. No partial verdict is shown, ever: a grade from half the gates is a different
claim than the one the screen makes.

---

## 5. Architecture

```
GUI thread ── posts ──▶ ResearchSession ──▶ assistant worker ──HTTPS──▶ model API
     ▲                        │              (tool loop; catalog served locally)
     │                        └──────────▶ job worker ──▶ run::run_backtest × N
     └── reads ── shared_ptr<const ResearchSnapshot>          (thread pool)
```

### 5.1 `ResearchSession`

A sibling of `Session`, not a part of it. `Session` replays one run interactively. Research runs
hundreds of complete backtests in batch, which is the shape `run/backtest.hpp` already exists for.

Same discipline as `Session`: commands posted from the GUI, one immutable snapshot published
behind a `shared_ptr`, nothing on the GUI thread waits.

`ResearchSnapshot` carries:

- the conversation, as rendered turns, including question cards and their answer state,
- the current form and provenance, and its validation errors,
- whether the assistant is available, and if not, why (no key, network, refused),
- job state and progress counts,
- the published Results, as `shared_ptr<const ResearchResults>`, and the analysis text so far.

`ViewMode` gains `Research = 2`. `F3` switches to it; `F2` keeps toggling Overview and Operator.
Research keeps its own layout state, per `SPEC_VIEW_MODES.md` §6. Switching modes never touches
either session.

### 5.2 Assistant worker

Runs the conversation as a tool loop against the model API, with the system prompt and the
tools from the assistant contract.

- **Catalog tools** (`search_securities`, `resolve`, `get_coverage`, `list_regimes`) are
  answered locally by the catalog (Part R1). They never touch the network.
- **`save_form`** validates against the schema in process and publishes the form. With
  `ready: true` the validation errors go back to the model, not to the screen.
- **`ask_user`** publishes question cards and ends the turn. The next turn starts when the user
  answers or types.
- After a run, the worker sends the form, provenance and Results, and streams the analysis into
  the snapshot.

The model id is configuration, not code. The key is read from `ANTHROPIC_API_KEY` with the same
hygiene as `live/credentials.hpp`: never logged, never in an error, never in a snapshot. Only
`key_hint` style redaction appears anywhere.

What is sent to the model: the prompt, the form, catalog answers, and Results. Never
credentials, account data, or file paths outside `runs/`. Say so in the view's settings, in
one sentence.

### 5.3 Network

`live/http.hpp` (WinHTTP) is reused, but not its allowlist. Add a second list in a new
`research/endpoints.hpp` holding only the model API host, and a client that refuses every host
not on it. The existing repository-wide host test is extended to cover both lists, and a new test
asserts the two lists are disjoint. The research client is not constructible with a broker
host, and the broker client is not constructible with the model host.

Rate limits are honoured, never worked around, per `DATA_POLICY.md` and the existing client's
`Retry-After` handling.

### 5.4 Job worker

Turns a validated form into a Results document:

1. **Bind** the form to `BacktestConfig` plus theory parameters. A form field the binder does not
   understand fails the job by name; it is never ignored.
2. **Sweep:** one `run_backtest` per point, on a pool sized from configuration.
3. **Select** on the in-sample window by `objective.metric`, honouring `min_trades`.
4. **Random-timing null, bootstrap, plateau, effective N:** per `RESULTS_SPEC.md`.
5. **Grade** under the current criteria version.
6. **Publish** the Results document, and write the run directory.

All randomness is seeded from the form's hash and the data snapshot, so the same form on the
same data produces byte-identical Results. Assert it.

### 5.5 Strategy and data

- **`theories/drawdown_entry`**: a theory with the parameters of the `drawdown_entry` template
  (reference, threshold, entry style, exit rules, re-entry), written against the existing
  `istrategy.hpp` and built like `sma_crossover`. Each future template is a theory.
- **Synthetic extension:** a feed that produces leveraged bars from an underlying and a cash-rate
  series, spliced at the target's first real bar, per the recipe formula in the catalog spec.
  Synthetic bars are marked so trades and the equity chart can say which part was simulated.
- **Grading**, the null, the bootstrap and the plateau ratio are implemented in the engine, and
  **independently** in `research/` in Python, from `RESULTS_SPEC.md`. They are cross-tested as
  `SIZING.md` and the parity work were: one written specification, two implementations, exact
  agreement on fixtures. A grade the two layers disagree on is a bug in one of them.

---

## 6. Parts

**R0 — Contracts in the repository.** Move the four contracts into `contracts/research/`. Add
a schema validator (nlohmann-json plus a small validator, or a vendored one; say which and why)
and tests that the example form and a fixture Results document validate.

**R1 — Catalog.** Built from the `.msb` cache, `SecurityMaster`, and a hand-maintained file of
fund details and regimes. `--catalog-info` on the headless CLI prints it, as `--cache-info`
does for the cache. Recipe validation numbers are produced by a script in `research/`, never
typed in.

**R2 — Assistant client.** The second allowlist, the client, the tool loop, the key handling.
Tested against a recorded transcript (the `recorded_feed` / `recorded_broker` pattern), so no
test touches the network.

**R3 — Job and grading.** `drawdown_entry`, the binder, the sweep, the gates, the Python
cross-implementation, and the determinism test. Headless first: `marketscan --headless
--research form.json --out runs/` produces a Results document without the GUI.

**R4 — The view.** States 4.1 to 4.4, the schema-driven form, the Results renderer, the
`EquityChart` reuse (see §7), Open in Operator.

**R5 — Runs on disk.** `runs/<run_id>/` holding `form.json`, `results.json`, the equity
curves, and the conversation transcript. The Recent list reads it. Nothing in it is committed.

Parts R0, R1 and R3 need no API key and no GUI. If the key or the toolchain is unavailable, the
work up to that point still lands.

---

## 7. The equity chart

There is still exactly one. `ChartModel` today borrows its vectors from a `SessionSnapshot`, and
`draw_drawdown_strip` takes the snapshot to read `StrategySnapshot::drawdown`. Both need a
second source.

The change: `ChartSeries` gains a borrowed `drawdown` pointer next to `values`, the strip reads
it from the model instead of the snapshot, and there are two builders, one from a
`SessionSnapshot` and one from `ResearchResults`. The draw functions don't change.
`tests/test_view_modes.cpp` must pass unchanged, and a new test asserts the Research builder
gives the same geometry and colours as the Session builder for the same curves.

Colour: the rule gets categorical slot 1 and the benchmark keeps its fixed hue, per
`SPEC_VIEW_MODES.md` §5. Synthetic shading and the out-of-sample rule use chrome tokens, never a
series or status colour.

---

## 8. Definition of Done

- A question typed in the Research view produces a form, questions only for missing or
  `x-ask: always` fields, a validated run, and a Results screen in the order of §4.3.
- No number in the Research view is computed by the view, with a test in the style of the
  view-mode tests asserting that every figure drawn is read from the Results document.
- The same form on the same data gives byte-identical Results.
- Engine and Python grading agree exactly on every fixture.
- No test touches the network. The research and broker host lists are disjoint, and asserted.
- With no API key, the view says why the assistant is off, and a hand-filled form runs.
- Open in Operator replays the selected parameters, and Operator's figures for that run match
  the Results' full-window figures exactly.
- Every result on `exercise_only` data says "Unverified" next to the grade.
- Screenshots of all three Research states committed beside the others.

---

## 9. STOP AND ASK

Prior triggers remain. Additionally:

- If the view needs a number the Results schema doesn't have, stop. The fix is a schema field,
  produced by the engine, not a computation in the renderer.
- If a form field cannot be bound to the engine as specified, stop and say which. Do not
  approximate it silently, and do not drop it.
- If engine and Python grading disagree and the cause is the specification being ambiguous,
  stop: the specification gets fixed first, then both implementations.
- If anything in the Research path would need to link `live/` or reach a broker host, stop.
