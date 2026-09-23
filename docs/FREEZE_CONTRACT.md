# The freeze contract

What "do not change any number" means, for a session that is told not to change
any numbers.

This exists because the literal version of that instruction is wrong in a way
that is easy to miss and awkward to argue with mid-session.

---

## Why this is a file rather than an amendment

Session G's brief set the rule this document amends, in its §1. **That brief is
not in the repository**, and neither is any other: no `SESSION_*.md` was ever
committed, and every session prompt in this project has been pasted into a
conversation and then existed nowhere. Session G's cleanup audit reported that as
drift in its §7, since two of its own prerequisites were files that do not exist.

So the amendment cannot be made where the rule was written. It is made here, in
the repository, where the next session can be pointed at it — which is the more
useful outcome anyway, because a contract that lives only in a prompt has to be
restated correctly every time and was not going to be.

**This applies to future freezes. Nothing in the repository changes because of
it.**

---

## The rule as it was written, and what it forbade by accident

> Every golden value, parity assertion, determinism hash and published number must
> be identical before and after.

Taken at its word, "published number" covers any number in any document. Session G
therefore found itself forbidden from correcting:

- a `STATUS.md` header claiming **367 tests** while its own body said **327**,
  when the real count was 466 — two numbers that could not both be right and
  neither of which was;
- a plugin ABI described as **"frozen at v1"** three sessions after
  `MS_ABI_VERSION` became `2`;
- a "last updated" line naming a session three sessions behind.

None of those is a measurement. Every one of them is a description of the
repository that had simply fallen behind it, and a freeze that protects them is
protecting the wrong thing: it makes the handoff document decay and calls the decay
compliance.

Session G corrected them and reported having done so. This is that judgement
written down as a rule, so the next session does not have to make it from scratch
under a brief that says otherwise.

---

## The two kinds of number

### Measured results — frozen absolutely

A number that came out of running something. Changing one means the run changed,
which means either a bug was introduced or a finding was made, and both are events
that stop a cleanup session rather than things it quietly absorbs.

| | examples |
|---|---|
| golden values in tests | the expected Sharpe in `test_metrics.cpp`, the sizing cases in `data/samples/sizing_cases.csv` |
| parity assertions | the micro-dollar equality between `theories/decile_parity` and `research/parity.py` |
| determinism hashes | the replay digests in `test_determinism.cpp` |
| published findings | Sharpe 2.414 and 1.810, the 1.927 threshold, the $606.07 divergence, 56 of 56 lookahead fills |
| anything derived from a run | every figure in `docs/figures/`, the measured event rates, the recording sizes, the latency distributions |

**A freeze on these is not a formality.** They are the only things in the
repository that can tell you the code changed behaviour, and a session that adjusts
one to make something pass has destroyed the instrument it was supposed to be
protecting. If one of them moves, that is the session's finding and its output is
a report, not a fix.

### Descriptive facts about the repository — corrected to measured reality

A number or name that describes the tree rather than a result. It is not evidence
of anything; it is a label, and a wrong label is simply wrong.

| | examples |
|---|---|
| counts of things in the repo | how many tests, how many files, how many lines |
| version identifiers | the ABI version, the pinned clang-format version, dependency baselines |
| session labels and dates | "last updated", "as of Session E" |
| paths and names | a file that moved, a function that was renamed |

**These may be corrected, and the correction is reported** — named in the commit
message and, if a session produces one, in its audit document. Corrected *to
measured reality*: the new value is one the session actually observed, not one it
assumed. Session G's corrected test count came from running the suite.

---

## Historical statements are not stale, and are not corrected

The distinction that does the most work, and the one easiest to get wrong once a
session has permission to fix descriptive facts.

A statement about what was true when a passage was written is **still true**. It
is not a description of the repository today and it is not drift.

> "No ABI change: `plugin_abi.h` is frozen at v1 and its vtable has no session
> hook, so the shim can only learn a session boundary by inference."
> — `docs/EXECUTION.md`

The ABI is at v2. That sentence is still correct, because it explains why the code
around it looks the way it does, and the constraint it names is the reason. Editing
it to say v2 would produce a sentence that is superficially current and actually
false: it would claim a decision was taken under conditions that did not apply.

Session G found four such statements — in `EXECUTION.md`, `IDENTITY.md`,
`BUILD_SPEC_2.md` and the Session C part of `STATUS.md` — and changed none of
them, recording the reasoning and the residual risk in `CLEANUP_AUDIT.md` §7.6.
That was correct and is now the rule.

**The test.** Ask what the sentence is doing. If it is telling the reader what is
true now, it is descriptive and may be corrected. If it is telling the reader why
something was done, it is historical and must not be. When a passage does both,
split it rather than choosing.

**The residual risk, named rather than solved.** A reader landing on one of those
historical lines cold can misread it as current. The defence is that each
authoritative statement lives in one identified place — the "What exists" table in
`STATUS.md`, `MS_ABI_VERSION` in the header itself — and that the historical ones
sit inside passages that are visibly about a past decision. That is weaker than a
mechanism and it is the honest position.

---

## Specifications are a third thing

`docs/specs/` records **what was asked for**. Not what was built, and not what
turned out to be possible.

A spec is never corrected, even when the thing it specifies did not happen. When a
requirement is superseded by events, it gets a dated editorial note beneath text
that is left exactly as written — as `BUILD_SPEC_2.md`'s Stooq requirement did in
Session G, under a note saying the endpoint grew a proof-of-work challenge that was
declined. The original asking is the historical record and altering it destroys the
only evidence of what the project set out to do.

---

## In one line each

- **A measured result changed** → stop, report it, do not fix it. That is a
  finding.
- **A descriptive fact is wrong** → correct it to something you measured, and say
  so.
- **A historical statement names an old constraint** → leave it alone.
- **A spec asks for something that did not happen** → note it beneath, change
  nothing above.
