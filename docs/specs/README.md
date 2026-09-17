# Specifications

The documents MarketScan was built from, verbatim and unedited.

They are here because code answers *what* and almost never answers *why not*. Half of what
shapes this repository is a list of things it was told not to do — no second code path for
backtest and live, no `double` for cash, no hash iteration where an order depends on it, no
optimistic fills by default — and none of that is recoverable from the source. A reader who
finds a decision strange should be able to check whether it was reasoned or accidental.

They are also the reference a future session re-grounds against. Their absence from the
repository was an open finding from the M0 audit for three milestones.

| | |
|---|---|
| [`BUILD_SPEC.md`](BUILD_SPEC.md) | The original. Sections 2 (the NOT-list) and 4 (the plugin ABI) still bind everything. |
| [`BUILD_SPEC_2.md`](BUILD_SPEC_2.md) | Data layer, universe, and the research harness. **Section 6 is superseded** — see below. |
| [`SPEC_2_M1B.md`](SPEC_2_M1B.md) | Order lifecycle and the backtest/live seam. Resolves audit escalation 2. |
| [`SPEC_VIEW_MODES.md`](SPEC_VIEW_MODES.md) | Dual GUI presentation modes. Scheduled as M9b; not started. |

**Where they disagree.** `BUILD_SPEC_2.md` §6 treats a ticker as a security's identity. The
Session A close-out replaced that model: a ticker is an attribute a security holds over a date
range, and a ticker that is reused by a different company is two securities. The later document
wins, and the reasoning is in [`../IDENTITY.md`](../IDENTITY.md).

These are historical records. They are not edited to match what was built; where the build
diverged, the divergence is recorded in [`../AUDIT.md`](../AUDIT.md) instead.
