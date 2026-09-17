# MarketScan — Addendum: M1b, Order Lifecycle and the Backtest/Live Seam

**Applies to:** `BUILD_SPEC_2.md`, Session A. Implement as **M1b**, immediately after M1
(session boundaries and `CrossSectionalStrategy`), before M2.
**Origin:** escalation #2 from the M0 audit — "the backtest/live seam is a claim without a
socket." This document is the resolution of that escalation.

---

## 1. The problem, restated correctly

The M0 audit found that `Engine` holds a `SimulatedClock` by value, so check 5 ("no second code
path for backtest vs live") passes literally but cannot be demonstrated — there is nothing to
swap and nowhere to put it. The audit's judgment not to add a virtual call to the event path for
a capability nothing uses was correct.

But the clock is the lesser half of that seam, and diagnosing it as the problem understates the
gap. The strategy-facing time boundary is already indirect: strategies read time through
`host->now()`, a function pointer in `MS_HostContext`. Whether `Engine` stores its clock by value
is an internal detail no strategy can observe.

**The real seam is the order lifecycle, and that one has a genuine untested surface today.**

In backtest, `submit_order` reaches the fill simulator and the fill comes back effectively
immediately. Live, it crosses a network, returns an id, and the fill arrives tens of milliseconds
later — partially, or not at all, or after a cancel races it. The ABI got the *shape* right:
`submit_order` returns an id and `on_fill` is a separate callback, so the interface is already
asynchronous. But because the simulator completes synchronously, **no strategy written against
this engine has ever experienced a pending order.** Every theory so far has been silently trained
on an assumption that breaks the first time it trades live.

That is worth fixing now, while two reference theories exist, rather than after a dozen do.

---

## 2. What I do NOT want

Spec 1 Section 2 and Spec 2 Section 2 remain in force. Additions:

- Do **NOT** implement latency as wall-clock time or anything else that breaks determinism.
  Latency is simulated, expressed in the engine's own time, and replay must remain
  byte-identical.
- Do **NOT** change `plugin_abi.h` in this milestone. Everything required here is achievable at
  v1; see Section 4 for the one thing that isn't, and why it waits.
- Do **NOT** make a non-zero latency the default for existing presets. Existing reference
  theories and their committed results must not move. Latency is opt-in per configuration, and
  at least one test preset turns it on.
- Do **NOT** model partial fills by emitting one fill and adjusting quantity after the fact. A
  partial fill is multiple `on_fill` callbacks summing to the filled quantity.

---

## 3. Requirements

### 3.1 Simulated submit-to-fill latency

- `IFillModel` gains a configurable delay between order acceptance and fill emission, expressed
  in simulated nanoseconds (an event-count mode is acceptable additionally, not instead).
- Default zero, so every result committed to date is unchanged. A test preset sets it non-zero.
- Fully deterministic. If jitter is wanted, it comes from the seeded host RNG, never from the
  system.
- Orders resting through the latency window are *live*: subsequent market events can move the
  price, and the fill is computed against conditions at fill time, not submit time. This is the
  entire point — an immediate fill at the submit-time price is the optimism the whole fill model
  exists to eliminate.

### 3.2 Partial fills

- The simulator may fill an order across multiple events when the available size at the touch is
  smaller than the order.
- Each partial emits its own `on_fill` with its own quantity and price. The ledger accumulates
  them. Average fill price is derived, never assumed.
- A test asserts that N partials summing to quantity Q leave the ledger in the same state as one
  fill of Q at the size-weighted average price.

### 3.3 Order state, wrapper-side

The C++ wrapper tracks every order it submitted and every fill it received, and exposes to
strategies: outstanding orders, filled quantity, remaining quantity, and time since submission.
No ABI change is needed — the wrapper already observes both sides of the conversation.

### 3.4 Cancel semantics

- `cancel_order` on a partially-filled order cancels the remainder only; fills already emitted
  stand.
- A cancel that arrives after the fill has been emitted is a no-op, logged distinctly.
- A test covers the race explicitly: submit with latency, cancel mid-flight, assert exactly one
  of {filled, cancelled} occurred and the ledger reflects it.

### 3.5 Strategy-facing tests

At minimum: a theory that submits and must behave correctly while the order is outstanding
(specifically, one that does not double-submit because it hasn't seen a fill yet — the single
most common live-trading bug); a theory that cancels a pending order; a theory that receives
partials. Replay determinism holds across all of them.

---

## 4. The one thing that needs ABI v2 — and why it waits

Cancellations, rejections and expirations produce no `on_fill`, so a strategy has no ABI-level
way to learn that an order terminated without filling. Properly solving that needs an
`on_order_status` callback, which is an ABI change.

**Do not make that change now.** Latency and partial fills — the large majority of the realism
gap — need no ABI change at all, and rejections are rare against a fill simulator. Terminal
non-fill notification is genuinely a live-execution concern and belongs to the milestone that
builds live execution.

**Do make the loader version-tolerant now**, so that migration is never a flag day: the loader
reads `ms_plugin_abi_version()` first (it already does), and branches to the vtable layout
matching the declared version rather than assuming the current one. A v1 plugin must keep
loading and behaving identically after v2 exists, with any newer callbacks simply never
delivered to it. Add a test that exercises the version branch with a synthetic v1 descriptor.

The host must never write past the end of the vtable a plugin compiled against. That is the
whole reason for the branch, and it is memory corruption if got wrong — which is exactly why it
gets built before it is needed rather than under deadline.

---

## 5. The clock

Template `Engine` on its clock type and add a compile-time test that instantiates
`Engine<SystemClock>` even though nothing runs it. Zero runtime cost, no virtual call on the
event path, and the seam is demonstrated to compile both ways instead of asserted.

The cost is pushing engine implementation into headers or doing explicit instantiation. If that
tradeoff looks materially worse up close than it does from here — compile times, code
organization — **do not do it**. Record it in `docs/AUDIT.md` as a known, named gap: the seam is
asserted and not demonstrated, it will be demonstrated when the live transport is built, and
that milestone must not resolve it with a mode flag. An honest recorded gap beats an
architecture bent around a capability nothing uses.

---

## 6. Definition of Done

Latency configurable and deterministic, default zero, non-zero in at least one test preset, with
replay byte-identical in both. Partial fills emit multiple callbacks and reconcile to the
weighted average. Wrapper-side order state available to strategies. The cancel/fill race tested.
The loader branches on declared ABI version with a test proving a v1 plugin survives. The clock
either templated with an instantiation test, or documented as a named gap in `AUDIT.md` — not
silently left.

Existing committed results for `buy_and_hold` and `sma_crossover` are unchanged, and a test
asserts it.

---

## 7. STOP AND ASK

All prior triggers hold. Additionally: if any requirement here turns out to need a
`plugin_abi.h` change that Section 4 did not anticipate, stop and say which one and why. Do not
make the change and do not work around it with a quantity-zero fill or any other in-band signal
— an ambiguous encoding of a terminal state is worse than the missing callback.
