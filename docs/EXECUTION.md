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
