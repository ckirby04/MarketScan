# MarketScan — Addendum: Dual View Modes

**Applies to:** `BUILD_SPEC_2.md` M9. Implement as **M9b**, after the parity test and the
Universe Inspector, before documentation.
**Prerequisite reading:** `BUILD_SPEC.md` Section 7 (GUI), `BUILD_SPEC_2.md` Sections 2 and 8.

---

## 1. What this is

Two presentation modes over the same running engine, switchable from Settings and by keyboard
shortcut:

- **Overview** — one thing at a time, legible from across a room, for showing someone what the
  system does and for checking in at a glance.
- **Operator** — the existing dense dashboard, for actually watching and debugging a run.

Both are live. Both read the same numbers. Neither is a "lite" version — Overview is a
*different question being asked*, not a subset of panels.

**Naming:** do not call the second one "Performance." In this application that word already
means strategy returns, and a settings toggle that reads "Performance view" in a tool that plots
performance is a genuine ambiguity. `Overview` / `Operator`. Rename if something better comes
up, but not to "Performance."

---

## 2. The non-negotiable rule

> **One snapshot. Two renderers. Zero duplicate computation.**

Both modes read from the same published engine snapshot. **No view computes a metric of its
own** — not a return, not a Sharpe, not a drawdown, not a rounding. If Overview needs a number
that doesn't exist in the snapshot, the number gets added to the snapshot and Operator can use
it too.

The failure this prevents: Overview showing 12.4% and Operator showing 12.1% because one
annualizes slightly differently, and nobody noticing until it's in a screenshot someone sent to
someone else. Enforce it with a test that asserts every metric surfaced in Overview is the same
object read by Operator, not a parallel calculation.

---

## 3. Overview — specification

The job: *is this working, and is it beating the market?* Everything that doesn't answer that
question is not in this view.

### 3.1 The hero figure

One number, ≥48px, in the UI sans, top of the view: **excess return vs. benchmark** — the thing
the entire project exists to determine. Directly beneath it in secondary ink, smaller: the
strategy's absolute return and the benchmark's, as context.

Not a gauge, not a one-bar chart, not a dial. A number.

### 3.2 The chart — ONE component, rendered at two densities

**There is exactly one equity chart in this application.** Not an Overview chart and an Operator
chart — one `EquityChart` component, one code path, one set of colors, instantiated in both
modes with a `density` parameter. This is the same anti-divergence principle as the snapshot
rule in Section 2: two chart implementations will drift, and the drift will show up in a
screenshot before it shows up in a bug report.

What `density` is allowed to change:

- Line weight of non-emphasized series (2px → 1px)
- Whether non-emphasized series keep their categorical hue or drop to the de-emphasis gray
- Direct-label density — every series, or only the emphasized one and the benchmark
- Margins, padding, font size, gridline density
- Legend placement, or its replacement by direct labels alone

What `density` must **never** change: the underlying data, which entity holds which color when
colored, the axis scale and normalization, or the geometry of any curve. A curve that looks
different between modes is a bug, not a style.

**Emphasis is a chart feature, not a mode feature.** The chart always knows which theory is
emphasized — in Operator that follows the selected leaderboard row, in Overview it follows the
strategy selector. Overview is simply the chart running at low density with the non-emphasized
series dropped to gray, because there one series is the point and the rest are context. Operator
at high density keeps the full categorical palette because there the series genuinely *are* the
subject. Same component, same colors, different presentation.

Constant in both modes:

- Normalized to the same starting capital, **one y-axis**. Never a second scale — if two
  measures don't share a scale, they get two charts or a common index. A dual-axis chart is not
  an acceptable output of this milestone under any framing.
- The benchmark is always drawn, always in its own fixed hue, never folded into the gray.
- Crosshair + tooltip on hover showing date, both values, and the spread between them.
- Recessive chrome: hairline gridlines, muted axis ink, no chart border.

### 3.3 The drawdown strip

A short underwater plot directly below the main chart, sharing its x-axis and its cursor.
Selected strategy against benchmark. This earns its place because drawdown is the thing a viewer
most needs to see and least thinks to ask for, and because a demo that shows only the up-and-to-
the-right curve is a dishonest demo.

### 3.4 The KPI row

Exactly four stat tiles. Not six, not eight. A KPI row of tiles, not a grouped bar chart:

1. **Information ratio** vs. benchmark — the risk-adjusted "am I beating the market" number.
2. **Sharpe**
3. **Max drawdown**
4. **Trade count** (with the period covered as its subtitle)

Each tile: label in muted ink, value large in primary ink, optional delta beneath. If a
sparkline fits, it's the same emphasis treatment as the main chart.

### 3.5 A strategy selector

A single unobtrusive control picking which theory is emphasized. Nothing else is interactive
in this view.

### 3.6 What is NOT in Overview

Blotter, positions table, order book, latency histogram, log console, correlation matrix,
plugin manager, transport controls beyond play/pause. If someone needs those, they switch modes.
Resist every instinct to add "just one more" panel here — the entire value of this view is what
it leaves out.

---

## 4. Operator — changes

Keep it as built. Three corrections that apply to it and to Overview alike:

**Color follows the entity, never its rank.** ImPlot assigns colors by plot order by default,
which means hiding a series from the legend repaints every series after it — a strategy silently
changes color and the leaderboard no longer matches the chart. Assign each theory a categorical
slot deterministically from its stable plugin `id` at load time, and hold it for the session
regardless of draw order, filter state, or how many theories are visible.

**Cap the categorical slots at eight, then fold.** Eight is the ceiling. A ninth theory does not
get a generated hue — it goes to "Other," or the view facets into small multiples. A generated
ninth color is indistinguishable from an existing one for a colorblind viewer and breaks the
whole scheme.

**P&L must not be red-versus-green alone.** This is the most common accessibility failure in
financial UIs and this app is currently committing it. Every gain or loss carries an explicit
sign or arrow glyph and a label in ink, with color as reinforcement only. Use the reserved
status palette for it — never a categorical series hue, so a P&L color can never be mistaken
for a strategy's identity.

---

## 5. Color system — pin these values

Both modes share one token table. Define it once, reference by role, never a raw hex at a call
site.

**Chart surface must be pinned to `#1a1a19`** (dark) / `#fcfcfb` (light). Contrast and CVD
results are only meaningful against the surface actually rendered on, so if the ImGui theme
paints a different plot background, either change it to these or re-run the palette validator
against the real one and report the result.

**Categorical slots**, in this fixed order — assign by stable id, never cycle:

| Slot | Hue | Dark | Light |
|---|---|---|---|
| 1 | blue | `#3987e5` | `#2a78d6` |
| 2 | orange | `#d95926` | `#eb6834` |
| 3 | aqua | `#199e70` | `#1baf7a` |
| 4 | yellow | `#c98500` | `#eda100` |
| 5 | magenta | `#d55181` | `#e87ba4` |
| 6 | green | `#008300` | `#008300` |
| 7 | violet | `#9085e9` | `#4a3aa7` |
| 8 | red | `#e66767` | `#e34948` |

**Status palette** (reserved — never reused as a series color):
good `#0ca30c`, warning `#fab219`, serious `#ec835a`, critical `#d03b3b`.

**Chrome and ink** (dark / light): primary ink `#ffffff` / `#0b0b0b`; secondary `#c3c2b7` /
`#52514e`; muted axis labels `#898781` both; gridline `#2c2c2a` / `#e1e0d9`; baseline `#383835` /
`#c3c2b7`.

**Text always wears ink tokens, never a series color.** Values, labels and legend text stay in
primary/secondary/muted; the colored mark beside them carries identity.

**Emphasis gray** for Overview's context series: the muted step, thin, no label.

If a light mode ships — worth it, because dark themes wash out on projectors and this view will
get projected — its values come from the light column above. It is a *selected* set of steps for
the light surface, not an automatic inversion of the dark one.

---

## 6. Mechanics

- Toggle in Settings, persisted alongside the layout, plus a keyboard shortcut (suggest `F2`).
- Switching modes never restarts, pauses, or perturbs the engine. The run continues; only the
  renderer changes. Assert this in a test.
- Each mode keeps its own ImGui layout state, so rearranging Operator doesn't disturb Overview.
- Overview must hold 60fps while the engine runs at maximum replay speed. It is doing less work
  than Operator; if it is slower, something is being recomputed per frame that should be read
  from the snapshot.
- Launch in Overview on first run, and remember the last used mode thereafter.

---

## 7. Definition of Done

Both modes render live off one snapshot; a test asserts no metric is computed twice.
Exactly one `EquityChart` implementation exists in the codebase, instantiated twice — a test
asserts the two modes produce identical curve geometry and identical entity-to-color assignment
for the same snapshot, differing only in the density parameters listed in 3.2.
Theory colors are stable across filtering, reordering and visibility changes — with a test that
hides a series and asserts the survivors' colors are unchanged. No dual-axis chart exists
anywhere in the application. Every P&L indicator carries a glyph or sign, not color alone.
Overview contains exactly one chart, one strip, four tiles, one hero figure and one selector —
and nothing else. Screenshots of both modes are committed and appear in the README.

---

## 8. STOP AND ASK

Spec 1 and Spec 2 triggers remain in force. Additionally: if implementing the shared-snapshot
rule would require recomputing a metric in the view layer, stop — that means the snapshot is
missing a field, and the fix is to add it to the snapshot, not to compute it twice. Say so
rather than working around it.
