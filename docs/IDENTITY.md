# Security identity and the survivorship guard

The load-bearing part of the data layer. Get this wrong and every number
downstream is fiction, quietly.

---

## A ticker is not an identity

It is an attribute a security holds over a date range. Conflating the two
produces two different failures, and they are not equally bad.

**A rename.** FB became META in 2022. One company, two names. Treating it as two
securities breaks a continuity that genuinely exists: a strategy cannot compute
a twelve-month signal across the change, and a position held through it looks
like a sale and a purchase.

**A reuse.** A ticker dropped in 2003 and reassigned to an unrelated company in
2011. Two companies, one string. Treating it as one **welds the first company's
price history to the second's** and manufactures a return that never existed —
usually an enormous one, because the two price levels have nothing to do with
each other.

The second is far worse, and the asymmetry drives every decision here:

| | Cost | Visible? |
|---|---|---|
| Split when you should not have | Lost history. A strategy has less to work with. | **Yes** — the strategy can say so |
| Merge when you should have split | A fabricated price series | **No** — nothing downstream can detect it |

**So when it is ambiguous, split.**

## What the engine guarantees

- `SecurityId` is permanent and never reused within a dataset. It is what
  positions, ledgers, universe membership and book state key on. **Nothing
  downstream keys on a string.**
- Resolution takes a date. `resolve(ticker, as_of)`.
- There is **no dateless overload**, and its absence is a compile error rather
  than a convention:

```cpp
SecurityId resolve(std::string_view ticker, Timestamp as_of) const noexcept;
SecurityId resolve(std::string_view ticker) const = delete;
```

"Which company is XYZ" has no answer without a date, so asking is impossible
rather than discouraged. `tests/test_universe.cpp` asserts the deleted overload
really is unusable, and that the dated one is — so the assertion is about the
deletion rather than about `resolve` not existing.

### The ABI did not change

`MS_HostContext.symbol_id(host, const char* ticker)` has no date parameter and
cannot gain one — the ABI is frozen at v1. It does not need one: **the host
knows what time it is**, so it resolves against `now()`. A strategy asking
mid-replay for a ticker gets the security that held it at that moment, which is
the correct semantics.

> **One consequence, and it matters.** The id returned for a given ticker string
> **can differ across a long replay** if that ticker was reassigned. Resolve
> once and hold the `SecurityId`; do not re-resolve a string every bar. This is
> also in [`theories/README.md`](../theories/README.md).

## How identity is inferred from free data

The membership history is ticker-keyed and cannot express identity, so it has to
be guessed at. The guess is **the length of the gap** in a ticker's membership:

1. `data/security_overrides.csv` is consulted first, and overrules everything.
2. A gap of **two years or more** splits the ticker into two securities.
3. A gap within six months of that threshold is kept, and flagged as a **near
   miss** — the calls that nearly went the other way are the ones worth
   reviewing.

Every one of those decisions — split or not — is written to
[`IDENTITY_DECISIONS.md`](IDENTITY_DECISIONS.md). The threshold is a judgment
call, not a derived constant, and the audit trail is what makes it reviewable
rather than magic.

```powershell
python scripts/fetch_sp500.py
marketscan.exe --headless --build-universe data/cache/universe/sp500_history.csv
```

On the real S&P 500 history — 2,720 snapshots, 1996 to the present — that
produces **1,254 securities from 53 gaps, 45 of them split**, before overrides.

### Renames are not inferred, and cannot be

From ticker-keyed membership data a rename is **indistinguishable** from one
name leaving and another arriving: the two events look identical and nothing in
the source says otherwise. Only something that knows about the company can tell
them apart.

So a renamed company currently appears as two securities. That is the safe
direction of the two errors, and it is still an error. The remedy is
`SecurityMaster::add_ticker`, which declares a rename explicitly, and a source
that carries corporate actions.

### When CRSP arrives

PERMNO is exactly what this module infers: a permanent security identifier that
survives renames and is never reused. At that point `SecurityId` becomes PERMNO
directly, a CRSP loader implements `IMembershipSource`, and **this entire
heuristic is deleted rather than adapted**. The interfaces are shaped so that
deletion is the whole of the change.

---

## The survivorship guard

A backtest run over today's index members, applied to history, is a backtest
over the companies that **survived** — and it reports a premium nobody could
have earned, because nobody knew in 1999 which names would still be there.

```powershell
marketscan.exe --headless --daily data/cache/stooq `
               --universe data/cache/universe/sp500_history.csv
```

The defence is **structural, not a rule a strategy author is asked to
remember**:

- The engine delivers a bar for security S on date D only if
  `S ∈ members_on(D)`.
- `symbol_count()` returns members *today*, so a strategy iterating
  `0..symbol_count()` cannot walk off the end of the index.
- `symbol_id(ticker)` returns nothing for a non-member.
- `position(id)` reads flat for a non-member — which closes a probe rather than
  hiding a balance, since anything held is liquidated the day it leaves.

`StrategyInstance` is the only path from an event to a plugin *and* the only
implementation of the host context that plugin has. Filtering there is not a
policy applied at a checkpoint; **it is the absence of a route.**

`--no-universe` turns it off, and is correct for intraday work and for a single
instrument. A run without a universe says so in its output, above the numbers,
because a cross-sectional result produced without one is survivorship-inflated
and a reader should not have to remember which kind of run they were looking at.

## Removal from the index

When a held security leaves the universe it is force-liquidated at the last
available price, the proceeds go to cash, and the trade is marked in the blotter
with `kForcedExitClientId` — a distinct marker, because a forced exit is not a
decision the strategy made and a blotter that showed it as one would credit or
blame a strategy for a trade it never chose.

> ### This is an optimistic approximation, and it is the biggest thing this data layer gets wrong
>
> A name leaves an index for two very different reasons: it was replaced by a
> bigger one, or it collapsed. The first liquidates near the last price. The
> second does not liquidate anywhere near it — real delisting returns are
> frequently −30%, −60%, or everything.
>
> **This models both as the first.** That is precisely where naive backtests
> hide their losses. When CRSP delisting returns arrive they replace this path,
> and until then a result produced over a universe with removals in it is
> flattered by an unknown amount, in one direction.
