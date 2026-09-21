# Execution realism

What happens between a strategy submitting an order and the fill coming back,
and how much of it is simulated.

Everything on this page is **off by default**. The default is an instant
simulator that fills in full, which is what every result in `README.md` and in
the quickstart notebook was produced under, and what the golden test pins to
the micro-dollar. Turning any of it on is a deliberate act, per run.

---

## The problem it exists to solve

The plugin ABI has always had the right shape. `submit_order` returns an id and
`on_fill` is a separate callback, so the interface has been asynchronous since
M4. But the fill simulator answered synchronously, and so:

> **No strategy ever written against this engine has experienced a pending
> order.**

Every theory here has been trained on an assumption that breaks the first time
it trades anywhere real — that after you submit, the position has changed. It
has not. The order is on a wire. It may fill in a moment, or partly, or not at
all, or a cancel may race it and lose.

That is worth fixing while two reference theories exist rather than after a
dozen do.

---

## Latency

```powershell
marketscan.exe --headless --data data/samples/synthetic_spy_1min.csv `
               --latency 250 --latency-jitter 100
```

An order with latency is **in flight**: accepted, alive, cancellable, and not
yet priced. Market orders wait like anything else. When it arrives, it is
filled against the book *then*, not the book at submission.

That last sentence is the whole point. An instant simulator hands a strategy
the price that was on the screen when it decided, for free, on every trade, and
the difference between that and the price it would really have got is
indistinguishable from alpha.

| Setting | Meaning |
|---|---|
| `submit_latency` | Fixed delay, in engine nanoseconds |
| `latency_jitter` | Uniform addition, drawn from the run's seeded generator |

**Both are simulated time.** Never wall-clock time, and jitter never comes from
the system: a delay measured from the machine would make a replay depend on how
busy the machine was, and a backtest that does that is not reproducible. The
jitter draw comes out of the same per-strategy stream as `random_uniform()`, so
a replay reproduces it exactly — asserted by `latency is deterministic, jitter
included`, which also checks that the jitter actually varies and actually
depends on the seed, because a jitter that quietly returned zero would satisfy
"the same seed replays" and nothing else.

## Partial fills

```powershell
marketscan.exe --headless --data ... --participation 0.25
```

`max_participation` caps what one event may fill at that fraction of the size
displayed at the touch. An order four times the displayed size arrives as
several fills across several events.

Each piece is **its own `on_fill` callback** with its own quantity and price.
Nothing is emitted and adjusted afterwards: a fill that was not true when it
was delivered is worse than no fill at all. The ledger accumulates them, and
the average cost is derived from what actually happened rather than assumed —
`the ledger's average cost follows the pieces, not the first one` moves the
price between two pieces and checks the position's average lands between them.

`partials reconcile to one fill at the weighted average` is the reconciliation:
350 shares in four prints and 350 shares in one leave the ledger in the same
place, to the micro-dollar.

## Order state, on the strategy's side

A strategy can ask what it has outstanding. These are maintained inside the
plugin, from the two things it already sees — what `submit()` returned and what
`on_fill()` delivered — so none of it required touching the frozen ABI.

```cpp
if (!has_pending_order(bar.symbol)) {          // the guard that matters
    submit_market(bar.symbol, MS_SIDE_BUY, 100);
}
```

| Call | Answers |
|---|---|
| `outstanding_orders()` | How many are working |
| `has_pending_order(symbol)` | Is anything working for this instrument |
| `pending_quantity(symbol)` | Signed size that will arrive if it all fills |
| `filled_quantity(id)` / `remaining_quantity(id)` | Progress of one order |
| `order_age(id)` | How long it has been alive |
| `cancel_pending(id)` | Has a cancel been asked for |

`has_pending_order` is the guard against the most common live-trading bug
there is: act on a signal, see no fill because the order is still in flight,
read the position as unchanged, and send it again. `an unguarded strategy
submits again before its order fills` reproduces that bug so that `a guarded
strategy waits for the fill it is owed` is measuring something.

### What this cannot know

A cancellation, a rejection after acceptance, or a DAY expiry produces **no
fill**, so nothing crosses back to tell the plugin the order is dead. There is
no `on_order_status` in ABI v1.

That is deliberate and it is not being fixed here. Latency and partial fills —
the large majority of the realism gap — need no ABI change at all, and
rejections are rare against a fill simulator. Terminal non-fill notification is
a live-execution concern and belongs to the milestone that builds live
execution. Encoding it in-band, as a zero-quantity fill or any similar trick,
would be worse than the missing callback: an ambiguous terminal state is a bug
that outlives the version that introduced it.

Until then an order that ends without filling stays outstanding inside the
plugin. Two explicit ways out:

- `forget_order(id)` — the strategy knows.
- `set_order_timeout(ns)` — the strategy would rather assume after a while.
  `abandoned_orders()` counts how often the assumption was made.

The bias is deliberate. Over-reporting a pending order makes a strategy too
cautious; under-reporting one makes it submit twice.

## Cancels

- Cancelling a **partially filled** order reaches the remainder only. Fills
  already emitted stand — a fill is a trade that happened, and no instruction
  sent afterwards makes it not have happened.
- A cancel that arrives **after** the order finished is a no-op, and is counted:
  `Broker::late_cancel_count()`. Live that is a race with a real winner, and a
  strategy losing it routinely is one whose logic assumes a speed it does not
  have.
- `exactly one of filled or cancelled happens, whenever the cancel lands` walks
  the cancel across the entire flight window and past it, and asserts at every
  offset that exactly one of the two outcomes occurred and the ledger agrees.

## ABI version tolerance

`include/marketscan/abi/abi_compat.hpp` is host-side only and does nothing
today: `MS_ABI_VERSION` is 1 and there is one layout.

It exists now because of how the vtable is filled in. The host declares an
`MS_StrategyVTable`, passes its address to `ms_plugin_create`, and the plugin
writes into it — writing *its own* idea of that struct. The direction matters:

| | |
|---|---|
| Plugin **older** than host | Writes fewer bytes than the host declared. Safe, given a zeroed struct and null checks. Supported. |
| Plugin **newer** than host | Writes **past the end** of the host's object. Silent memory corruption, before any callback runs. Refused, on the declared version, before create. |

The loader now branches on the declared version rather than requiring equality,
`trim_to_version` nulls anything past what that version defines, and a
`static_assert` on `sizeof(MS_StrategyVTable)` fails the build if a slot is ever
added without updating the table. A v1 theory will keep loading and behaving
identically after v2 exists, and simply never have the newer callbacks
delivered.

---

## The assumed exit

A name that leaves the index has to be got out of, and neither layer of this
project knows what that actually cost. Both substitute something, and the shared
limitation has a name so that the two halves can point at the same thing:

> **The assumed exit.** Nothing here models a *measured* delisting return. Both
> layers substitute something, they now substitute the same thing under the same
> named setting, and the setting says which substitution produced a given number.

### The switch

`DelistingReturnModel` exists on both sides, spelled identically:
[`include/marketscan/exec/delisting.hpp`](../include/marketscan/exec/delisting.hpp)
and [`research/delisting.py`](../research/delisting.py). Two settings:

| | what it does |
|---|---|
| `last_price` | Liquidate at the final quoted price; apply no delisting return. **The default.** It is what the engine has always done, and it is what a backtest does when nobody has asked the question. |
| `apply_true_return` | Apply the delisting return **where the data supplies one**. A security with no row is liquidated at the last price either way: a missing row is a coverage limit, not a return of zero, and `DelistingReturns::has` is what keeps the two apart. |

In the engine the return is booked by `Ledger::apply_delisting_return` as its own
line rather than as a fabricated fill — no order, no counterparty, no commission,
because what happened is that a position turned out to be worth something other
than its last print. Cash and realized P&L move by the identical integer, so the
ledger invariant holds across the path without being told about it, and
`delisting_pnl()` tallies separately how much of a result came from an assumption
rather than from a trade.

`last_price` stays the default because changing a default silently moves every
number measured under it. Turning this on is a decision somebody makes.

### What the assumption is worth

Measured, on the committed churn parity fixture — thirty names over six months, 29
departures, seven of them held when they happened, and a generator whose delisting
returns are known by construction:

| | `last_price` | `apply_true_return` |
|---|---|---|
| total return | **+8.63%** | **-20.78%** |
| Sharpe | **+1.81** | **-1.33** |
| delisting returns booked | $0.00 | -$29,400.58 |

The assumption is worth **29.42 points of return and 3.14 of Sharpe** on this
fixture, and it does not merely move the result — it flips its sign.

The gap is larger than the adjustments themselves ($29,417.43 against
$29,400.58). The extra $16.85 is compounding: a delisting return moves cash,
decision-time equity is cash plus the marked book, and every rebalance after the
first booked loss sizes a smaller book. Small on six months because most of the
loss lands in the final month; it is the larger of the two terms over a long
panel, and the assertion is therefore the inequality rather than the figure.

[`PATHOLOGIES.md`](PATHOLOGIES.md) measures the same assumption from the research
layer alone, on its own panel, and reports it independently.

### Both layers, asserted equal

`tests/test_parity.cpp` replays the churn fixture through the engine under each
setting and compares it to a curve `research/parity.py` produced from the same
bytes. The two agree **to the micro-dollar under both models**, and every booked
adjustment is reconciled against the reference row for row — name, date, position,
mark, return, and micro-dollars.

That ordering is the point of Session D Part 2. Under `last_price` the adjustment
is zero, so agreement is agreement about *when* each layer noticed a departure and
at what price it liquidated. Under `apply_true_return` both layers apply the same
known return, so agreement is additionally agreement about *how much* the exit was
worth. The old fixture could not separate those: the departure return used to be
the departing name's final bar, which made "liquidate at the last price the book
has" capture the loss by construction, and both layers could agree perfectly while
being wrong in the same direction about the size of the exit they assumed.

### What is still assumed

**CRSP delisting returns replace the substitution with a measurement.** Until
then, `apply_true_return` on real data has nothing to apply, and every number
either layer produces over a universe with departures in it carries `last_price`.
On synthetic data the generator supplies the truth, which is what makes the
measurement above possible and also what limits it: the bimodal draw by cause —
distress with mass near total loss, voluntary exits at a small premium — is a
modelling choice, not an estimate of anything.

---

## The assumed instant

A decision is taken from some information and acted on at some price, and the two
cannot be the same instant. Session D Part 3; the shorthand for the limitation
**and** for the check that now enforces its absence.

### The triple

Every fill records three timestamps, and `TradeRecord` carries all three:

| | |
|---|---|
| `decision_ts` | The information horizon of the decision: the timestamp of the last event **in that symbol** that the strategy had *finished* being shown when it submitted. `StrategyInstance` keeps the per-symbol bookkeeping and records it after the plugin callback returns, so an order submitted from inside `on_bar(E)` is credited with what the strategy knew before E. |
| `price_ts` | `OrderBook::last_update` for that symbol at the instant the fill model priced it — the market state the price came out of. |
| `ts` | The fill. |

Per symbol, not per session, because no notion of a session is needed: what makes
a fill dishonest is that the strategy had already been shown the price it traded
at, in that instrument.

> **The invariant.** `price_ts > decision_ts` for every fill the strategy asked
> for. `StrategyInstance::audit_instants` tallies the blotter into honest,
> **simultaneous** (the lookahead), stale, unrecorded and forced-exit, and
> `tests/test_parity.cpp` asserts it on both committed fixtures.

An unrecorded triple counts as its own category rather than as honest: an
unverified fill is not a verified one. **Forced exits are excluded**, and the
exclusion is named rather than quiet — a forced exit is not a decision, the
security has stopped printing, and there is no later price for it to fill
against. That is *the assumed exit*, above, measured in its own right.

### What it found

Recorded, not argued. With the triple in place and nothing else changed:

| | fills | honest | simultaneous |
|---|---|---|---|
| `parity_panel.csv` | 56 | **0** | **56** |
| `parity_churn_panel.csv` | 55 | **0** | **55** |

Every strategy fill in both committed fixtures traded at exactly the price its
decision was made from. `STATUS.md` had this down as *predicted and not observed*
— the reading of the engine was right, and the evidence had been a curve that
happened to agree.

The mechanism: `Engine::step` applies an event to the book before dispatching it,
so a strategy reacting to a print and the fill model pricing its order see the
same market. That is right for a strategy reacting to a print. But
`IStrategy::advance_session` fires `on_session_close(N)` from inside the dispatch
of the **first bar of session N+1**, so an order submitted there prices against
session N's book for every symbol except the one whose bar rolled the session —
which prices against N+1's. One symbol early, twenty-nine simultaneous.

### The fix, and what it cost

`CrossSectionalStrategy` now carries a **target book**: `on_cross_section` states
what it wants to hold, and each name's target becomes an order when that name next
prints. Every fill therefore lands at the first price its symbol printed after the
decision, uniformly, with no symbol special. A target whose symbol never prints
again is abandoned rather than carried — a target from two sessions ago, filled
now, is a decision acting on information it no longer has.

No ABI change: `plugin_abi.h` is frozen at v1 and its vtable has no session hook,
so the shim can only learn that a session ended by being handed the next one's
first bar. The fix belongs on the strategy side of that boundary, and that is
where it is.

It moved every committed parity number:

| | equity before | after | Sharpe before | after |
|---|---|---|---|---|
| `parity_expected.csv` | $106,660.71 | $107,038.64 | +1.313 | +1.414 |
| churn, `last_price` | $111,739.45 | $108,633.80 | +2.414 | **+1.810** |
| churn, `apply_true_return` | $81,635.69 | $79,216.37 | -1.140 | -1.326 |

Caught by the invariant above, and by `tests/test_parity.cpp`'s churn-parity case
before that.

**The sign is not uniform, and that is not a defect.** The churn-free path got
$377.92 *better* without the lookahead. Trading a decile ranking one session later
is a different strategy, not a strictly worse one, and on a single synthetic path
the noise can go either way — the same caveat as *A note on the numbers* below.
What the lookahead bought was not a uniform edge; it was a systematic
misstatement.

**What is not ambiguous is the Sharpe on the churn fixture.** +2.414 cleared the
`NULL_SHARPE_THRESHOLD` of 1.927 that [`FALSE_DISCOVERY.md`](FALSE_DISCOVERY.md)
derived from a 1,512-run null sweep; +1.810 does not. On this fixture the
lookahead was worth 0.60 of Sharpe and it was the whole difference between a rule
that looks distinguishable from noise after adjusting for the size of the search
and one that does not.

### What is still assumed about the instant

A daily bar is one price, so "the next price this symbol printed" is the next
session's close. A real order sent after a close fills somewhere in the next
session, not at its close, and nothing here models where. The invariant says the
fill is *later* than the decision; it does not say it is *realistic*.
---

## A note on the numbers

Turning latency on changes the results, and not always for the worse. On the
committed sample `sma_crossover` reports a *smaller* loss with 250ms of latency
than without it, and its slippage column goes negative.

That is not a finding. It is one synthetic price path.

**Why it happens here.** The sample is geometric Brownian motion. Price drift
over a flight window of a few hundred milliseconds is zero-mean by
construction, so latency makes slippage measured against the decision price
*two-sided* — sometimes the market moves your way while the order is in the
air, sometimes against — with unchanged expectation and added variance. Over
twenty decisions a favourable draw is unremarkable.

That is not a claim about the simulator, so it was measured rather than
asserted. `scripts/latency_sweep.py` runs the comparison across 200 seeds and
reports the distribution; the result is in
[`LATENCY_DISTRIBUTION.md`](LATENCY_DISTRIBUTION.md). A distribution centred
away from zero would have meant the simulator was doing something systematic
and would have been a bug worth finding.

**Why it does not generalise.** On real data latency acquires a sign, and the
sign is a property of the strategy rather than of the engine. A signal that
fires *on* a move gets systematically worse fills during flight in a market
with short-horizon continuation, and systematically better ones in a
mean-reverting market. Synthetic GBM has neither, which is precisely why it
shows neither.

So the sign and the magnitude of the latency effect on real data are an
empirical property of the strategy and its horizon. **Deferred, not
concluded.** Nothing in this repository is entitled to an opinion about it
yet, and the free data stack is not the thing that will settle it.

---

# Live execution

Everything above was written against a simulator. This section is about the day
a venue answered instead, what that changed, and what it did not.

## The seam, and how it is kept honest

There is one `Engine`. It is templated on a clock, and that template parameter
is the whole of the backtest/live distinction on the time side. On the execution
side the distinction is `IBroker`: `Broker` prices orders through a fill model,
`LiveBroker` sends them to a venue, and `StrategyRuntimeConfig::broker` decides
which one a run gets. Nothing below that point can tell or ask.

There is no mode flag, no `#ifdef`, and no branch on "are we live" in the
engine, in `StrategyInstance`, or in any plugin. That is not a promise; it is
what makes the equivalence test below possible, and the equivalence test is what
would notice if it stopped being true.

**Where the asymmetry actually lives.** `Broker` answers inside `submit()`.
`LiveBroker` does not: it sends the order, returns, and the answer arrives later
on a socket. That is the same code path with real latency in it, and it is why
the simulator grew a submit-to-fill delay and an `on_order_status` callback
before there was anything live to need them. A strategy that copes with a
pending order in replay copes with one here.

**One thing that is genuinely different and is not hidden.** `LiveBroker::submit`
makes a synchronous HTTPS request on the calling thread, which is the engine
thread. An order submission therefore stalls event dispatch for the duration of
a round trip -- about 60ms on this machine when the connection is warm. In
replay the same call returns in microseconds. That is a real property of this
implementation, not a modelling choice, and a strategy submitting on every event
would feel it.

## The market data feed

`LiveMarketDataFeed` is an `IMarketDataFeed` like `CsvFeed`. It owns a thread
that reads a WebSocket, parses messages, and pushes events into an
`SpscRingBuffer`; the engine thread pops them through `next()`. `next()`
returning false means "nothing right now" rather than "end of stream", which was
already in `IEventSource`'s contract before there was a live feed to need it.

**The parser is shared with replay on purpose.** `live/stream_parser` turns a
socket message into events, and both the live feed and `RecordedFeed` call it.
If each had its own, the equivalence test would be comparing two parsers, and a
divergence in rounding or in which message kinds are recognised would surface as
a strategy disagreement and be debugged in the wrong place.

### What IEX is and is not good for

The free tier streams IEX: one venue's own prints, roughly two per cent of
consolidated volume. That is enough to prove the seam and to exercise an order
lifecycle. It is **not** enough to draw a conclusion about liquidity, spread, or
fill quality from, and nothing in this repository is entitled to one.

Three specific limits, all measured rather than assumed:

- **Trades carry no aggressor.** The feed does not say who crossed the spread.
  `MS_Trade::aggressor` is set to buy as a placeholder, matching `CsvFeed`'s
  behaviour for a file with no side column. Any statistic that reads it on this
  feed is reading a constant.
- **Extended hours are thin to the point of empty.** On 2026-09-21 at 16:12 ET,
  a 30-second capture of AAPL, SPY, TSLA, NVDA and QQQ produced 254 events --
  *all of them QQQ*. Four of the five most liquid symbols on the list produced
  nothing at all. A strategy pointed at one of those four sees an empty market,
  which is what happened the first time the equivalence test ran.
- **Quote sizes are shares, not round lots.** This was checked rather than
  assumed, and the assumption was wrong. The first draft multiplied by a
  hundred. In a 45-second session, bid and ask sizes ranged 40 to 1880 while
  trades ranged 1 to 200 shares; read as lots those quotes would be 4,000 to
  188,000 shares at the touch on a two-per-cent venue against forty-share
  trades. `kQuoteLotSize` is 1.0 and carries the measurement.

### Bars are not late, they are interval-stamped

A one-minute bar is stamped at the *start* of its minute and cannot be sent
until the minute ends, so it always arrives about sixty seconds behind the ticks
around it. Folded into a jitter statistic that reads as a transport failure: the
first capture reported **60,236 ms backwards** and looked like a broken feed
when it was three bars behaving exactly as documented.

So `MonotonicGate` counts them apart, and the skew statistic excludes bars
entirely. One minute bar is enough to swamp three thousand quotes.

### Measured clock skew

Local clock minus exchange timestamp, over trades and quotes only, measured
before the monotonic gate touches anything:

| session | ticks | mean | min | max |
|---|---|---|---|---|
| 2026-09-21, 45s, AAPL/SPY/MSFT | 3,212 | -186.4 ms | -192.7 ms | -147.1 ms |
| 2026-09-21, 25s, AAPL/SPY | 1,850 | -180.8 ms | -190.8 ms | -93.5 ms |

The sign says this machine's clock is roughly 0.19s *behind* the exchange. The
`/v2/clock` round trip bounds the same quantity independently: a warm request
put it between -255 ms and -198 ms.

This is transport and clock offset together. Separating them needs a time source
this machine does not have, and a number that pretended otherwise would be
invented. It is reported, never applied as a correction: events keep their
exchange timestamps.

**The measurement point matters.** An earlier version computed replay skew from
timestamps the monotonic gate had already raised, and a replay of a session
reported a mean 0.2 ms adrift of the live run it was reproducing -- two
definitions of one number. There is now one, in `RecordedFeed`, at the same
point in the pipeline the live feed measures at, and a test holds it there.

## The recorder

A recording is NDJSON: one header line, then one line per socket message, each
carrying the raw text, an arrival timestamp and a sequence number. Nothing is
reinterpreted on the way in, so a parser bug found later can be fixed and the
recording reparsed.

It needs its own format because `CsvFeed` decides what a file holds from its
header -- bid/ask means quotes, open/high/low/close means bars, a bare price
means trades, one kind per file. A live stream interleaves all three and the
interleaving is the part that matters.

A reconnect is written into the file as a marker, so a replay sees the gap
rather than reading straight across it.

**A recording is market data and is never committed.** See
[`DATA_POLICY.md`](DATA_POLICY.md) section 7. The equivalence test generates its
own and deletes it; `--keep` exists for debugging a divergence and `.gitignore`
still stops it reaching a commit.

## The order lifecycle, against a real venue

`lifecycle_exercise` is **not a strategy**. It submits marketable and
non-marketable limits, cancels some, and provokes one rejection, so that the
code handling those states has been run against a venue rather than only against
a simulator. It has no edge and loses money by design.

Measured on 2026-09-21, paper account, QQQ, 150-second session in extended
hours, with the trade-updates connection dropped at 60 seconds on purpose:

| | |
|---|---|
| decisions | 10 |
| submitted to the venue | 10 |
| filled | 4 |
| partially filled | 0 |
| cancelled | 5 |
| rejected by the venue | 1 |
| cancels that lost their race | 4 |
| trade-stream reconnects | 1 |
| orders reclaimed by client order id | 4 |
| stale updates dropped after the reclaim | 3 |
| reconciliation | positions agree with the venue |

The rejection came back in the venue's own words: `ioc orders are only accepted
during market hours`. The four lost cancel races are the case worth having run
-- a cancel that arrives after the fill is a cancel that was correctly sent and
correctly too late, and a strategy whose cancels are routinely late is assuming
an execution speed it does not have.

**Partial fills were not exercised live.** Every order is one share, and one
share does not partially fill. The partial-then-expiry path is covered by
`tests/test_live_broker.cpp` and by the simulator, and not by a venue. That is a
gap, stated rather than papered over.

### Order identity survives the connection

Every order carries a client order id this process derives from its own engine
order id *before* it sends anything: `ms-<run>-<order id>`. The venue's id is
not known until the submit response arrives, and a dropped connection can
deliver a fill before it -- so an order is found by the string we chose, never by
the one the venue chose.

That is what makes the reclaim work. After a reconnect the broker re-reads the
venue's orders, parses the client ids, and knows exactly which engine order each
one is. An id belonging to another run reads as *not ours* rather than as a
match, because adopting one would book somebody else's fills into this ledger.

### Reconciliation is loud

The local `Ledger` and the venue are compared, and a difference is reported with
both numbers and never corrected. This is not decoration. On 2026-09-21 a run
reported *this run says 6 shares of QQQ, the venue says 9*; the three-share gap
was traced, order by order, to a previous run that had **died mid-session** with
three fills booked at the venue and none locally. A ledger that had quietly
adopted the venue's answer would have made the numbers agree and hidden the fact
that a run had crashed holding a position.

"I could not check" is kept distinct from "they agree":
`Reconciliation::agreed()` requires that the check ran and produced no error.
`ms_live positions --flatten` exists because a check that is permanently red is
a check whose next real failure nobody can see.

## The equivalence test

Run one plugin binary live against the paper venue while recording the market
data; then run **the same file** against that recording; compare the decisions.

**What is compared.** The order sequence, and for each order the symbol, side,
quantity, type, time in force, limit price, the strategy's own client order id,
and the timestamp of the event in that symbol the strategy had finished being
shown when it submitted. Exactly, including the prices -- both sides computed
them from the same doubles in the same order, so a tolerance here would hide a
real difference rather than absorb a rounding one.

**What is not compared.** Fills. Live, the venue decides; in replay the
simulator does, against a book rebuilt from the recording. Those differ and are
supposed to.

**The result**, 2026-09-21, QQQ, 150 seconds, extended hours, with a forced
trade-stream drop:

```
live events      463
replay events    463
live decisions   10
replay decisions 10

EQUIVALENT: 10 decisions identical across live and replay.
```

**What that establishes.** The event pipeline is the same on both sides: the
same bytes produce the same events, in the same order, with the same timestamps
and the same symbol ids, and a strategy fed them reaches the same decisions.

**What it does not establish.** That a fill-dependent strategy would agree. One
would not -- live it saw the venue's fills and in replay it saw the simulator's
-- and that is a true statement about such a strategy rather than a defect in
the seam. `lifecycle_exercise` is deliberately written so its decisions are a
function of the event stream alone, and says so in its own header. Timing is
also not reproduced: a replay runs as fast as the engine will go, so a strategy
that decided something because of how long something took would diverge.

**A pass can be vacuous, and is refused when it is.** Two empty decision
sequences compare equal. The first three attempts at this test passed that way,
and the harness now reports the decision count, the per-symbol event counts, and
a `VACUOUS` failure with a non-zero exit rather than a green line.

### One harness bug worth recording

An early run reported 609 live events against 610 replayed, and the decisions
matched anyway. That is worse than a failure: the live half was not draining the
events still queued when the feed stopped, and the next undrained event to
trigger an order would have been reported as a divergence that was really a bug
in the test. The live half now drains its tail through the strategy before
comparing.

## What this machine taught us about WinHTTP

Two findings, both measured, because the first guess was wrong both times.

**Synchronous WebSocket receive does not honour any timeout.** Not the
session's, not the request's, and not `WINHTTP_OPTION_RECEIVE_TIMEOUT` on the
socket handle -- that last one is accepted and then ignored, which is worse than
being refused. A receive on a socket with nothing to say blocks until the
connection dies.

A probe held two authenticated sockets open with nothing subscribed and sat in
the first receive for **seven minutes** with a thirty-second timeout configured.
The market data socket never showed this, because a subscribed socket is never
quiet; the trade-updates socket is silent whenever no order is working, so its
pump thread could not be joined and every shutdown that had to join it hung.
`WebSocket::request_close()` ends the conversation from the controlling thread
instead; closing two silent sockets now takes 111ms.

**It was not TLS concurrency, which is what it looked like.** Three TLS
connections from one process, two of them long-lived sockets, on a machine with
a TLS inspection proxy, is a plausible story and it was wrong. `ms_live probe`
measures it:

```
  0 sockets open                     301ms 60ms 60ms 60ms
  1 socket (market data)             59ms 57ms 58ms 63ms
  2 sockets (+ trade updates)        245ms 74ms 65ms 58ms 79ms 73ms
  POST /v2/orders, 2 sockets open    status 422 in 128ms
  closing both sockets ... 111ms
  0 sockets again                    75ms 60ms 57ms 57ms
```

The first request in each row pays for the TLS handshake through the local
proxy; every later one is about 60ms. Concurrency costs nothing measurable. The
probe stayed in the repository because it is how the next person avoids the same
two wrong guesses.

## Operational limits of the free tier

- **One market data stream connection per account.** A second one is refused
  with `406: connection limit exceeded`, and the slot is not released
  immediately -- consecutive runs need roughly a minute between them. The feed
  treats 406 as fatal and does not retry, because retrying a refusal is exactly
  what [`DATA_POLICY.md`](DATA_POLICY.md) section 1 forbids.
- **Market orders are rejected outside 09:30-16:00 ET**, and IOC orders with
  them. A limit order with `extended_hours` set trades 04:00-20:00 ET;
  `LiveBrokerConfig::allow_extended_hours` asks for that, off by default,
  because a strategy that has not been told it is trading a thin book should not
  be.
- **Wash-trade protection refuses an order opposite a live one in the same
  symbol.** One leftover close order silently turned a whole session into
  thirteen rejections and nothing else, which is why `--flatten` cancels working
  orders before it closes anything.

## The trading calendar

Session boundaries come from the broker, through `/v2/calendar`, not from a
hard-coded 09:30 to 16:00. That is wrong about eleven days a year and wrong in
the direction that costs money: half days close at 13:00, holidays do not open,
and the dates move.

The response gives local wall-clock times with no zone, so `eastern_utc_offset`
implements the post-2007 US rule -- second Sunday in March to first Sunday in
November -- rather than pulling in a timezone database the project does not
otherwise need. Verified against a real fetch: 2026-09-21 through 2026-10-01
returned nine sessions, correctly skipping the weekend, each 13:30 to 20:00 UTC.

`MarketCalendar` keeps "the market is closed" distinct from "this calendar
cannot say", because a runner that treated an unfetched December as a holiday
would be wrong in a way nothing would notice.
