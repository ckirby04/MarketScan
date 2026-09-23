# MarketScan — Session H: Closing the Inventoried Gaps

> **Committed retroactively**, after the session it governed, because the
> convention in [`README.md`](README.md) did not exist when the session started.
> The text below is the brief as given, unedited. Session H departed from it in
> two places and both are recorded in the commit messages rather than here: the
> freeze contract became a new file because the `SESSION_G.md` it was meant to
> amend does not exist, and Part 3's second gap was closed in Part 1 by deriving
> the counts rather than only asserting them. This is the last brief that will be
> committed after the fact.

**Run after Session G.** Short session — four small behaviour changes and two documentation fixes,
none of which could ride along with G's freeze.

**Prerequisite reading:** `docs/AUDIT.md` (the vacuous-pass law and the three inventoried gaps),
`docs/CLEANUP_AUDIT.md` (the deliberately-not-done list), `SESSION_G.md` §1.

---

## 1. What I do NOT want

All prior NOT-lists remain in force. Additions:

- Do **NOT** change any measured result. Golden values, parity assertions, determinism hashes and
  every published finding stay identical. This session adds guards; it does not move numbers.
- Do **NOT** make any new assertion a warning. A guard that logs and continues is a guard that
  will be scrolled past.
- Do **NOT** blanket-ban the Python layer from using the C++ bindings. The bindings exist and are
  legitimately used; only specific modules need enforced independence. See §3.
- Do **NOT** pick up anything from Session G's "needs a decision" list. Those are decisions, and
  they get made deliberately rather than absorbed into a cleanup follow-up.
- Do **NOT** start anything that depends on CRSP or a price vendor.

---

## 2. Part 1 — Summaries that assert their own arithmetic

`OrderTracker` once reported *"128 submitted, 124 accepted, 0 rejected"* while four orders were
plainly refused. Nothing was lost — every refusal was in the log — but a summary that states
something untrue is its own bug, and this one was caught only because a human did the subtraction.

**The shallow fix** is to assert the identity: submitted equals accepted plus rejected, fills plus
partials reconcile against the blotter, and so on for every related group of counts any `ms_live`
subcommand prints. Hard failure, not a warning.

**The deeper fix, preferred where it is practical:** derive the summary from the log or the ledger
at print time rather than maintaining parallel counters that can drift from what they count. A
counter incremented at a call site is a second source of truth, and the bug was precisely that the
second source stopped being updated when the code around it moved. Where a counter must exist for
performance or structure, assert it against the derived value rather than trusting it.

Audit every summary the tool prints — `ms_live` in all its subcommands first, then any other
executable that reports counts.

**Definition of Done:** every printed group of related counts either derived from one source or
asserted to reconcile; a test that deliberately desynchronises a counter and confirms the
assertion fires; the original `128/124/0` case covered by a named regression test.

---

## 3. Part 2 — Enforce the deliberate duplication mechanically

Session G found that the rationale for keeping the C++ engine and the Python research layer
independent was documented nowhere, and fixed that — while noting honestly that **nothing
mechanical enforces it**. Someone can still collapse the two layers and every test will pass,
because the tests measure agreement, and a single shared implementation agrees with itself
perfectly.

Close it: a test that fails if the specific modules whose independence *is* the measurement import
the pybind11 bindings, directly or transitively.

Scope it precisely. Name the modules — the sizing implementation and the parity reference
implementation at minimum — and assert their import graph excludes the C++ bindings. Do not ban
the bindings from `research/` generally; they exist for good reasons and are used legitimately
elsewhere.

The failure message should say why, not just what: that these two implementations are deliberately
separate, that collapsing them makes the parity test tautological, and where the rationale is
written down.

**Definition of Done:** guard implemented and scoped; a test proving it fires when a forbidden
import is introduced; `AUDIT.md`'s "nothing mechanical enforces it" admission updated to say what
now does, and what still does not.

---

## 4. Part 3 — Apply the three inventoried vacuous-law gaps

Session G Part 3 inventoried three places where the law is not yet applied and deliberately left
them unfixed, because applying them is behaviour change. Apply them now, one commit each, each
with a test that confirms the new guard fires.

---

## 5. Part 4 — Instance five

The law acquired a fifth instance during the commit series that documented it: the first
`ms_live` executable-against-executable comparison ran two processes that never started, and would
have printed `IDENTICAL`. It was caught only by the script's own byte-count assertion — that is,
by a check that the mechanism had produced any output at all.

Add it to `docs/AUDIT.md` as instance five, and note where it occurred. A law that catches itself
being violated while being written down is the most persuasive form the argument takes, and it is
also the clearest evidence that noticing the pattern does not confer immunity to it.

---

## 6. Part 5 — Fix the freeze contract

`SESSION_G.md` §1 required that every number in `STATUS.md` be identical before and after. Taken
literally that forbade correcting a header claiming 367 tests while the body said 327, and an ABI
described as "frozen at v1" three sessions after it reached v2.

Amend the contract to distinguish:

- **Measured results** — golden values, parity assertions, determinism hashes, published findings
  and any figure derived from a run. These are frozen absolutely.
- **Descriptive facts about the repository** — test counts, ABI version, session labels, file
  paths. These may be corrected to measured reality, and the correction is reported.

Historical statements describing what was true at the time are not stale and are not corrected —
`SESSION_G` got that discrimination right and the amended contract should say so explicitly.

This amendment applies to future freezes; nothing in the current repository changes because of it.

---

## 7. STOP AND ASK

All prior triggers hold. Additionally:

1. If deriving a summary rather than counting it would change a printed number, stop — that means
   a published figure was wrong, which is a finding and not a cleanup.
2. If the import guard cannot be scoped without either banning legitimate binding use or failing
   to catch the collapse it exists to prevent, report the tension rather than picking a side.

---

## 8. What "done" looks like

No summary this tool prints can state something its own numbers contradict. The one architectural
invariant that had no mechanical enforcement now has some, and the documentation says exactly how
much. Five instances of the vacuous law are recorded in one place, including the one that happened
while writing it down.

After this the engineering backlog is empty of anything that does not need data — and the project
is waiting on Walton rather than on itself.
