# The `drawdown_entry` rule

The written specification of the `drawdown_entry` template, step by step. Two implementations
follow it and neither follows the other: the engine's built-in strategy
(`src/marketscan/research/drawdown_entry.cpp`) and the research layer's reference
(`research/drawdown_entry.py`). They are cross-tested on committed fixtures for exact agreement on
every order, the same arrangement as [`docs/SIZING.md`](../../docs/SIZING.md).

A change to either implementation cites the step below that it implements. A step that turns out
to be ambiguous is fixed here first, then in both.

Two departures from [`SPEC_RESEARCH_VIEW.md`](../../docs/specs/SPEC_RESEARCH_VIEW.md) §5.5, which
is a historical record and is not edited:

- The rule is a **built-in strategy**, not a `theories/drawdown_entry` DLL. The job instantiates it
  hundreds of times per question with different parameters, and a strategy whose output is graded
  should not be swappable on disk between the run and the grade.
- Only **`execution.fill: next_close`** can be honoured. A daily feed has no opening quote, so
  `next_open` has nothing to fill against; the binder refuses it by name rather than
  approximating it.

---

## Inputs

One instrument's daily bars, indexed `t = 0, 1, ...`, each with a close `c[t]`. Bars before the
form's start date are **warmup**: they update every running quantity below but are never
decision bars.

Parameters, all from the form:

| parameter | meaning |
|---|---|
| `reference` | `all_time_high`, `rolling_high` with `window` bars, or `since_last_exit` |
| `tranches` | One or more `(level, fraction)` pairs, in percent. A single entry is one tranche. |
| `exits` | Any of `recover_to_high`, `take_profit p`, `stop_loss p`, `trailing_stop p`, `time_stop n` |
| `reentry` | Whether a new campaign may start after an exit |
| `reserve` | Cash reserve percent used in sizing (step 6). Not a form field; the binder sets it. |

## Execution model

**A decision is made at the close of bar `t` and fills at the close of bar `t + 1`.** That is
the form's `execution.fill: next_close`.

In the engine, the order is submitted on bar `t` with a nonzero `submit_latency`, so it is in
flight when bar `t + 1`'s quote arrives and fills against that quote. The engine's lookahead
triple then shows `decision_ts < price_ts` for every fill, which is asserted. Submitting on bar
`t + 1`'s quote instead would fill at a price the strategy had already been shown.

A buy fills at bar `t + 1`'s ask and a sell at its bid. Quotes are synthesized from the close with
the run's spread: `half = c * (spread_bps * 1e-4 * 0.5)`, `ask = c + half`, `bid = c - half`.

An order decided on the last bar never fills.

What the engine does at the fill, which the rule does not choose but a second implementation has
to reproduce to agree with it:

- **Price.** For a quantity `q` within the quote's displayed size, `avg = (px * q) / q` with `px`
  the ask or bid; a buy fills at `max(avg, ask)` and a sell at `min(avg, bid)`.
- **Cash.** A buy costs `llround(price * q * 1e6) + commission` micro-dollars and a sell credits
  `llround(price * q * 1e6) - commission`.
- **Refusal.** A buy whose cost is greater than the cash on hand is refused whole.

## Running quantities, updated on every bar including warmup

1. **Reference high `H[t]`:**
   - `all_time_high`: the largest close in `c[0..t]`.
   - `rolling_high`: the largest close in the last `window` bars ending at `t`, or in all of
     `c[0..t]` if there are fewer.
   - `since_last_exit`: the largest close from the bar on which the most recent exit **filled**
     through `t`. Before the first exit, the same as `all_time_high`.
2. **Drawdown** `d[t] = 100 * (1 - c[t] / H[t])`, in percent, never negative.

## A campaign

A campaign starts with the first buy decision while flat and ends when its exit fills. It
records:

- `B`: equity at its first buy decision, in micro-dollars,
- `H_entry`: `H[t]` at its first buy decision,
- which tranches have filled,
- the bar of its first fill, the average fill price of its buys, and the highest close since its
  first fill (the fill bar included).

The average fill price is `sum(price * quantity) / sum(quantity)` over the campaign's buy fills,
each sum accumulated in fill order.

## Steps on each decision bar `t`

3. **While holding, check the exits first.** In this fixed order, the first that holds is the
   exit reason:
   1. `recover_to_high`: `c[t] >= H_entry`
   2. `take_profit p`: `c[t] >= avg * (1 + p / 100)`
   3. `stop_loss p`: `c[t] <= avg * (1 - p / 100)`
   4. `trailing_stop p`: `c[t] <= peak * (1 - p / 100)`, `peak` being the highest close since the
      first fill
   5. `time_stop n`: `t - first_fill_bar >= n`

   If one holds, sell the whole position. No buy is considered on the same bar.
4. **Otherwise, consider buys.** None if a campaign has ended and `reentry` is false. Otherwise the
   triggered tranches are those not yet filled in this campaign with `d[t] >= level`. None
   triggered: nothing to do.
5. **Start the campaign** if there is none: record `B = equity` and `H_entry = H[t]`.
6. **Size one order for all triggered tranches together:**
   ```
   commission = the run's per-order commission, micro-dollars
   wanted     = llround(B * (sum of triggered fractions) / 100)    micro-dollars
   budget     = min(wanted, cash - commission)                      micro-dollars
   estimate   = c[t] * (1 + reserve / 100)
   shares     = floor((budget / 1e6) / estimate)
   ```
   `llround` rounds half away from zero. Each line is evaluated in the order written, in double
   precision, and the fractions are summed in tranche order. If `shares < 1`, no order is sent,
   the tranches stay unfilled, and a campaign started at step 5 on this bar is abandoned as if it
   had never started.
7. **Submit** a market buy for `shares`. The tranches it carries are filled when the order fills.
   If the broker refuses it (a gap up that the reserve did not cover), they stay unfilled and are
   considered again on later bars. A campaign with no position and no order in flight after a
   refusal has ended without an exit, and `reentry` does not apply to it.

## Order tags

Every order carries a `client_order_id`, so a fill can be attributed without a lookup:

- buy: `0x100 | mask`, where bit `k` of `mask` is tranche `k`, counting from 0 in form order,
- sell: `0x200 | reason`, with reasons 1 `recover_to_high`, 2 `take_profit`, 3 `stop_loss`,
  4 `trailing_stop`, 5 `time_stop`.

## The benchmark

Buy-and-hold is this rule with one tranche at `level = 0, fraction = 100` and no exits. It buys
on the first decision bar and holds to the end, paying the same spread, commission and reserve.
It is not a separate implementation, so the two cannot differ in anything but the rule.
