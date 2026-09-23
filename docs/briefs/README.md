# Session briefs

The instructions each session was given, committed for provenance.

---

## The rule this directory exists to serve

**A brief is ephemeral instruction. A standing rule is not. When a brief contains
a rule that outlives it, the rule is promoted to a citable document and the brief
keeps only the pointer.**

A brief says: do these things, in this order, this time. That is worth keeping as a
record of what was asked, and it is the wrong place for anything that must still be
true next month. A rule left in a brief has to be restated correctly every time it
matters, by someone who may not have the brief — and this project has the scar:

- Session G's brief contained the **protected list**, the enumeration of things
  that look like cruft and are load-bearing. It lived in a prompt for two sessions.
  It is now [`PROTECTED.md`](../PROTECTED.md).
- Session G's brief contained the **freeze contract**, and its literal wording
  forbade correcting a `STATUS.md` header that contradicted its own body. It is now
  [`FREEZE_CONTRACT.md`](../FREEZE_CONTRACT.md), amended.
- Session G's brief asserted the **deliberate duplication** was documented in
  `BUILD_SPEC_2.md` §2 and in a `SESSION_D.md`. It was documented in neither, and
  nowhere else either. It is now a section of [`AUDIT.md`](../AUDIT.md).

Three standing rules in one brief, none of them in the repository, all three
discovered by a session that went looking. The general fix is not "commit the
briefs" — that preserves the prompt without making the rule findable. It is to
promote the rule *and* commit the brief.

### This is the same failure twice

[`AUDIT.md`](../AUDIT.md) recorded it at M0, as the first of two escalations:

> **`BUILD_SPEC.md` is not in the repository.** Spec 2 names it as prerequisite
> reading. It has never been committed; Spec 1 was delivered as a message and only
> its conclusions reached disk. The constraints are being honoured, but a reader
> cannot check them against the source, and neither can a future session.

That was closed at the Session A close-out by committing the specs to
[`specs/`](../specs/). The specs were fixed and **the briefs were not**, so the
identical problem ran for another seven sessions and produced the identical
symptom: Session H's brief cited `SESSION_G.md` as prerequisite reading, and
Session G's cited `SESSION_D.md`, and neither existed.

The M0 finding also anticipated the right response to a lost document, and it is
the one this directory takes: *"Reconstructing it from memory would produce a
document that looks authoritative and is not, so it has not been attempted."*

The lesson the first escalation did not carry far enough is that **"prerequisite
reading that is not in the repository" is a class, not an instance.** Specs,
briefs, and anything else a future session is told to read first all belong on
disk, and the rule above is the part that keeps the *content* findable once the
document is.

### What has been promoted so far

| standing rule | where it lives now |
|---|---|
| Things that look like cruft and are load-bearing | [`PROTECTED.md`](../PROTECTED.md) |
| What "do not change any number" means | [`FREEZE_CONTRACT.md`](../FREEZE_CONTRACT.md) |
| The vacuous-pass law, and its five instances | [`AUDIT.md`](../AUDIT.md) |
| Why the two layers stay independent | [`AUDIT.md`](../AUDIT.md), "Deliberate duplication" |
| What may and may not be fetched, and from where | [`DATA_POLICY.md`](../DATA_POLICY.md) |
| The sizing arithmetic both layers implement | [`SIZING.md`](../SIZING.md) |
| Which identity calls were judgement and which were measurement | [`IDENTITY_DECISIONS.md`](../IDENTITY_DECISIONS.md) |

### The test for whether something should be promoted

Ask whether it is still true after the session ends. "Subscribe to five liquid
symbols and run for 300 seconds" is not. "A notional cap must refuse rather than
clamp, because a cap that silently resizes can no longer tell you a sizing bug
exists" is — and that one is in [`EXECUTION.md`](../EXECUTION.md) rather than only
in the message that first said it.

---

## What is here, and what is missing

**Only Session H's brief survives verbatim.** Every earlier one was pasted into a
conversation and then existed nowhere: not in the repository, not in a file, not
recoverable. That is the gap this directory closes going forward and cannot close
backwards.

| session | brief | subject |
|---|---|---|
| A – F2 | **lost** | the chassis, the identity model, the research layer, the false-discovery work, the live execution seam, venue responses in the recording |
| G | **lost** | audit and cleanup |
| H | [`SESSION_H.md`](SESSION_H.md) | closing the inventoried gaps |

The missing briefs are recorded as missing rather than reconstructed. Writing out
what a lost instruction "probably said" would produce a document indistinguishable
from a real one and worth less than nothing — the same objection this project makes
to a regenerated fixture.

What those sessions asked for is partly recoverable from other evidence, and that
is where to look instead: `git log` has a commit message per part, and
[`STATUS.md`](../STATUS.md) has a section per session describing what each owed and
delivered. Neither is the instruction, and both are honest about being a summary of
it.

---

## Convention for future briefs

1. Commit the brief verbatim as `SESSION_<id>.md`, before the work starts, in its
   own commit.
2. Do not edit it afterwards. If the session departs from it, the departure belongs
   in a commit message and in the session's own report, not in a retrospective
   rewrite of the instruction. A brief that has been edited to match what happened
   is no longer provenance.
3. Before closing the session, identify anything in the brief that will still be
   true afterwards, and promote it. Add it to the table above.
