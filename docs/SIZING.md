# Position sizing

How a cross-sectional strategy turns a ranking into share counts.

This document exists because the rule did not. It lived twice in code — once in
`theories/decile_parity/decile_parity.hpp` and once in `research/parity.py` — and
the two copies disagreed, silently, in the eighty-fourth session of a
hundred-and-twenty-six-session fixture. Neither was wrong against a written
specification, because there was no written specification. There is now, and both
implementations are written against *this page* rather than against each other.

> **Both layers implement this independently.** The research layer does not call
> into the engine and never will: `BUILD_SPEC_2.md` §2 keeps it a separate
> implementation, and the Linux `research` CI job exists to enforce that. Two
> implementations of one written rule, cross-tested, is the point — a shared
> implementation would agree with itself and prove nothing.

---

## The rule

Given a cross-section at a session close, a ranking over it, and an existing
book:

```
 1.  side        = floor(count * decile)                     whole names, per leg
 2.  equity      = cash + Σ notional(close_i, position_i)    micro-dollars, integer
 3.  book        = (equity / 1e6) * gross                    dollars, double
 4.  per_name    = book / (2 * side)                         dollars, double
 5.  target_i    = ±per_name / close_i                       shares, double
 6.  delta_i     = target_i - position_i                     shares, double
 7.  submit if   |delta_i| >= 1e-9
```

with `notional(price, quantity) = llround(price * quantity * 1e6)`, rounding half
away from zero, exactly as `include/marketscan/core/money.hpp` defines it.

Every step names its type because the types are where the two implementations
drifted apart. Steps 1 and 2 are integer; 3 through 6 are IEEE-754 doubles
evaluated left to right in the order written; step 7 compares an absolute value.

---

## Step 2 is the one that mattered

**Decision-time equity is computed from the cross-section the decision is based
on.** Every position is marked at *that session's* close — the same closes the
ranking used — plus cash.

The engine offers `equity()`, and the C++ implementation used to call it. That
was the bug, and it is a more interesting bug than a rounding difference:

`equity()` returns the ledger's live marked equity, and the ledger marks each
position at the last price the book has for it. A `CrossSectionalStrategy` is
handed its session at `on_session_close`, which the shim fires while the engine is
dispatching **the first bar of the next session** — and the engine applies an
event to the book before any sink sees it. So at the moment the decision is made,
one symbol — whichever bar happened to roll the date — is already marked at
tomorrow's close, and every other symbol at today's.

That is a price from the future in the decision, so:

- The two layers disagreed whenever that symbol was held and its price had moved,
  which is why the divergence was intermittent and why it took 84 sessions and
  four rebalances to surface.
- The engine's own sizing was, in a small way, **looking ahead**.

Computing equity from the cross-section fixes both at once. It needs nothing
outside the frozen ABI: `cash()`, `position()`, and the closes already in hand.

> **A strategy should not ask the host what it is worth at a moment the host
> defines differently than the strategy does.** The cross-section is the
> strategy's definition of "now"; the book is the engine's. They are one event
> apart, and one event is a whole session for daily bars.

---

## Shares are fractional, deliberately

Nothing rounds to whole shares. `submit_market` takes a `double` quantity and
the ledger carries positions as doubles, so a target of 306.4172 shares is
submitted as 306.4172 shares.

This is a modelling choice and it is worth naming rather than discovering:

- **For:** rounding to whole shares introduces a second rounding rule into a
  path that already has one, and it would have to be specified at the boundaries
  (what does a 0.5-share target do?) with both layers agreeing about it. The
  parity assertion is about the accounting, and whole-share rounding adds noise
  to the thing being measured without making the accounting more correct.
- **Against:** no real venue fills a fractional share of most instruments, so a
  book built this way is very slightly cheaper to rebalance than a real one.

The cost is bounded by half a share per name per rebalance, which on a
thirty-name book at a few hundred shares each is under a tenth of a percent of
turnover. If whole-share sizing is ever wanted, it belongs here as a named rule
with its boundary behaviour written down, implemented in both layers, and covered
by the cross-test — not as a `std::floor` added to one of them.

---

## Step 1: the decile boundary

`side = floor(count * decile)`, and both legs take `side` names: the top `side`
by rank and the bottom `side`. With 30 names and a decile of 0.1 that is 3 and 3,
leaving 24 untouched.

If `side == 0` or `2 * side > count` the strategy does nothing that session. Too
few names cannot form two disjoint sides, and a leg that overlapped the other one
would be long and short the same security.

**Ties are broken by symbol id, ascending**, after sorting by the ranked value
descending. The tiebreak exists so the decile boundary cannot depend on an
ordering the two implementations do not share, and `research.parity.assert_no_ties`
checks the committed fixture never actually needs it — a tie would put a name on
opposite sides of the boundary in the two runs and look like an accounting bug.

---

## Step 7: the minimum trade

A delta below `1e-9` shares is not submitted. Without it the two implementations
would agree about every number that matters and disagree about how many fills
happened, because a target recomputed from an equity that moved by one
micro-dollar produces a delta of about 1e-12 shares.

---

## Order of operations, and why closes come before opens

Positions being closed are submitted first, then the long leg, then the short
leg. A rebalance that opens before it closes can be refused for want of cash, and
an order the fill model declines is a divergence between the two implementations
that has nothing to do with sizing.

---

## What checks this

| | |
|---|---|
| `research/tests/test_sizing.py` | The rule, in Python, over a grid of (equity × weight × price) including adversarial inputs. |
| `tests/test_sizing.cpp` | The same grid, in C++, asserting the same share counts. |
| `tests/test_parity.cpp` | End to end: both implementations over the committed fixture, asserted **equal**, with no tolerance parameter anywhere. |

The grid is generated from one table of cases that both suites read, so the two
cannot drift onto different inputs. Adversarial cases are in it on purpose:
prices near zero, equity at rounding boundaries, weights that do not divide
cleanly, and the exact case that produced the 84-session divergence.
