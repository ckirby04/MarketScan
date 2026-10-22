# The research job

How a validated Strategy Form becomes a Results document: binding, the sweep, windows, metrics,
selection, the gates and the grade. [`RESULTS.md`](RESULTS.md) says what each part of the result
means; this says exactly how each number is computed, so that two implementations can agree on
it.

The engine's implementation is `src/marketscan/research/job.cpp`. The research layer's,
`research/grading.py`, starts from the engine's runs (the equity curves and fills, exported as a
trace) and recomputes everything after them independently: metrics, selection, the null, the
correction, the plateau, the gates and the grade. Neither derives from the other.

**Arithmetic.** Sums are taken in index order, one term at a time. `sqrt` is correctly rounded on
every platform and the two implementations agree on it exactly. `pow` is not guaranteed to, so
quantities computed with it (CAGR, anything derived from CAGR, and the corrected p-value) are
compared to a relative `1e-12` and everything else exactly. Where such a quantity feeds a
decision -- selection by CAGR, a gate on the corrected p-value -- the decision must still agree. Integers, counts, verdicts and the grade are always exact.

---

## 1. Binding

The binder turns a form into runs, or refuses it with every reason at once, each naming the form
path it is about. It never approximates a field it cannot honour.

Refused, until the part of the system that would honour them exists:

| path | refused when | because |
|---|---|---|
| `/template` | anything but `drawdown_entry` | the only template built |
| `/universe/assets` | more than one asset | the template is single-instrument |
| `/universe/assets/0/synthetic_extension` | present | needs the catalog (R1) |
| `/universe/price_adjustment` | `total_return` | the bar store holds no dividends |
| `/cash/yield` | anything but `none` | there is no rate series |
| `/execution/fill` | `next_open` | a daily feed has no opening quote |
| `/robustness/walk_forward` | present | not built |
| `/strategy/entry/tranches` | more than four | the rule's limit |
| any data | a symbol with no series, or no bar on or after the start date | |

Accepted, with a known limit, rather than refused, because both are about what is reported and
neither changes a number that is:

- `robustness.regime_breakdown: true` gives an empty `stats.regimes`: named periods come from the
  data catalog, which does not exist yet. The view says so.
- `robustness.parameter_surface: false` reports `stats.parameter_surface` as null. The surface is
  still computed, because the `multiple_testing` and `plateau` gates measure it.

Bound values: `date_range.start` is the first decision day; bars before it are loaded as warmup,
the most recent `warmup_days` of them, or all earlier bars when `warmup_days` is 0. Bars after
`date_range.end` are dropped. `costs.slippage_bps` is the half-spread: the synthesized spread is
`2 * slippage_bps`. `costs.commission_per_trade` is charged per order. The reserve is 2%.

## 2. The sweep

Every numeric strategy parameter given as a `sweep` or `values` is a **swept dimension**,
identified by its form path. Dimensions are ordered by path, as strings. A sweep's values are
`min + i * step` for `i = 0, 1, ..., n - 1` with `n = floor((max - min) / step + 1e-9) + 1`; a
`values` list is taken in the order given. A single-valued dimension is not a sweep.

Points are the Cartesian product, enumerated with the **last** dimension varying fastest. A point's
index is its position in that enumeration. More than 500 points is refused.

The benchmark is run once: buy-and-hold of the primary benchmark (`benchmarks[0]`) over the same
bars, costs and reserve (DRAWDOWN_ENTRY.md, "The benchmark").

Every run -- sweep point, benchmark, bootstrap path -- is checked by the engine's own instruments
before anything is graded. A ledger whose accounting identity did not hold, or a fill whose
`price_ts` is not later than its `decision_ts`, refuses the whole job as an engine bug: a grade
built on either would be a statement about the bug.

## 3. Windows

`s` is the first decision bar, `e` the last bar. With `oos_split` a date, `o` is the first bar on
or after it. The windows are bar ranges:

- `full`: `[s, e]`
- `in_sample`: `[s, o - 1]`
- `out_of_sample`: `[o, e]`

A window's **levels** are equity at bars `a'..b`, where `a' = a - 1` when `a > s` and `a' = a`
otherwise, so a window's first return is the one earned on its first bar. Its returns are
`r_i = L_i / L_{i-1} - 1` for `i = 1..m`, `m` being the number of levels less one.

A window with fewer than two levels has every metric null except `n_entries`.

## 4. Metrics

With `L` the levels and `r` the returns of a window, `D` the calendar days from the date of `L_0`
to the date of `L_m`, and nulls as noted:

| metric | definition |
|---|---|
| `total_return_pct` | `100 * (L_m / L_0 - 1)` |
| `cagr_pct` | `100 * (pow(L_m / L_0, 365.25 / D) - 1)`; null if `D = 0` |
| `volatility_pct` | `100 * sd * sqrt(252)`, `sd` the sample standard deviation of `r` (divisor `m - 1`); null if `m < 2` |
| `sharpe` | `mean(r) / sd * sqrt(252)`; null if `m < 2` or `sd = 0` |
| `sortino` | `mean(r) / dd * sqrt(252)`, `dd = sqrt(sum(min(r_i, 0)^2) / m)`; null if `dd = 0` |
| `max_drawdown_pct` | `100 * min_i(L_i / P_i - 1)`, `P_i` the running maximum of `L_0..L_i` |
| `max_drawdown_days` | the longest stretch, in calendar days, from the date of a running maximum to the date equity first regains it after falling below it, or to the date of `L_m` if it never does; 0 if equity never falls below a running maximum |
| `calmar` | `cagr_pct / abs(max_drawdown_pct)`; null if `max_drawdown_pct = 0` or CAGR is null |
| `ulcer_index` | `sqrt(sum((100 * (L_i / P_i - 1))^2) / (m + 1))` over `i = 0..m` |
| `time_in_market_pct` | `100 * (bars t in a'+1..b with exposure) / m` |
| `n_entries` | campaigns whose first buy filled on a bar in `[a, b]` |
| `win_rate_pct` | `100 *` the share of trades closed in the window with `return_pct > 0`; null if none |
| `avg_trade_return_pct` | mean `return_pct` of trades closed in the window; null if none |
| `worst_trade_pct` | least `return_pct` of trades closed in the window; null if none |

`mean(r)` is `sum(r) / m`. The sample standard deviation is
`sqrt(sum((r_i - mean)^2) / (m - 1))`.

**Exposure.** Bar `t` has exposure when a position was held over the return into it: after the
bar of a campaign's first buy fill, up to and including the bar of its sell fill.

**Trades.** A trade is a campaign. Its cost is the sum over its buy fills of
`price * quantity + commission`, and its value is `price * quantity - commission` of its sell
fill, or `close * quantity` at the last bar if still open. `return_pct = 100 * (value / cost - 1)`.
A trade is closed in a window if its sell fill's bar is in it, and is `open` otherwise.

A trade that straddles the split is counted by where it *started* for `n_entries` and for its
`window` in `stats.trades`, and by where it *closed* for the win rate, average and worst trade.

The rest of a trade's row in `stats.trades`: `entry_price` is `sum(price * quantity) / sum(quantity)`
over its buy fills, without commission; `exit_price` is its sell fill's price, or the last close if
open; `holding_days` is calendar days from its first buy fill's bar to its sell fill's bar, or to
the last bar; `max_adverse_pct` is the least of `100 * (c_t / entry_price - 1)` over the bars from
its first buy fill to its sell fill (or the last bar), and never above 0; `exit_reason` is the
sell tag's reason, or `open`. These describe the trade and feed no gate, and the research layer
does not recompute them.

## 5. Selection

The objective is `objective.metric` on the in-sample window, or on `full` if there is no
out-of-sample split. A point is **eligible** when its in-sample `n_entries` is at least
`objective.min_trades`. The selected point is the best eligible point; if none is eligible, the
best of all points. **Better** means higher, except for `ulcer_index`, where lower is better.
`max_drawdown_pct` is negative, so higher means shallower. A null objective is worse than any
number. Ties go to the lower index.

## 6. The random-timing null

**Generator.** SplitMix64, seeded once per job with the first 8 bytes of the SHA-256 of the
canonical form (the form's JSON with `prompt` and `provenance` removed, keys sorted, no
whitespace), read little-endian. Each call:

```
state += 0x9E3779B97F4A7C15
z = state
z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
z = (z ^ (z >> 27)) * 0x94D049BB133111EB
return z ^ (z >> 31)
```

all in unsigned 64-bit arithmetic. `draw(n)` is `next() % n`.

**Holding lengths.** For the selected point over the full window, each trade's length is the
number of bars with exposure it contributed. `k` trades, lengths `H_1..H_k`, `M = e - s` bars that
can carry a return.

**One alternative.** Shuffle a copy of the lengths by Fisher-Yates from the last element down,
swapping element `i` with element `draw(i + 1)`. Draw `k` gaps with `draw(M - sum(H) + 1)` each,
sort them ascending, and place trade `j` to start after bar `s + g_j + sum(H_1..H_{j-1})`: it has
exposure on the next `H_j` bars.

**Its metric.** The null and the selected point are compared on a frictionless curve, so that the
comparison is about timing alone: `V_s = 1`, and `V_t = V_{t-1} * (1 + c_t / c_{t-1} - 1)` on a bar
with exposure, `V_t = V_{t-1}` otherwise. The selected point's own exposure gives its curve the
same way. The metric is `objective.metric` over `full` on these curves.

**Report.** 1,000 alternatives, drawn in order. The strategy's percentile is
`100 * (count below + 0.5 * count equal) / N`. The p-value is `(1 + count at or above) / (1 + N)`.
"Below" and "above" use section 5's *better*, so a null alternative counts as below a numeric
strategy figure, and two nulls are equal.
`null_p5`, `null_p50` and `null_p95` use the percentile rule below. With no trades, the null is
not drawn: percentile 0, p-value 1.

**Percentiles.** Of sorted `x_0..x_{n-1}` at fraction `q`: `h = (n - 1) * q`, `l = floor(h)`,
`x_l + (h - l) * (x_{l+1} - x_l)`, or `x_l` when `l = n - 1`. Nulls are dropped first.

## 7. The multiple-testing correction

Only when something is swept. Each point's full-window daily returns form a series. Series with
zero variance are dropped. Of the remainder's correlation matrix, `N_eff` is the fewest of its
eigenvalues, taken largest first, whose sum reaches 95% of their total; at least 1. The corrected
p-value is `1 - (1 - p)^N_eff`.

## 8. The plateau ratio

Only when something is swept. A point's **edge** is its in-sample objective minus the primary
benchmark's in-sample value of the same metric, negated for `ulcer_index`. The neighbours of the
selected point are the points that differ from it by one step, up or down, in exactly one swept
dimension. The ratio is the median of the neighbours' edges divided by the selected edge; null if
there are no neighbours, any edge is null, or the selected edge is not positive. The median of an
even count is the mean of the middle two.

## 9. The outcome range

The selected point is re-run on 1,000 resampled paths of its instrument, generated after the null
from the same generator, and so is buy-and-hold of that same path. On a path the benchmark is
always the instrument itself, whatever `benchmarks[0]` is: the question a path answers is whether
the timing beat holding what was timed, and resampling a second instrument jointly would need
dates the two series may not share.

**A path.** Warmup bars are kept as they were. From bar `s` on, closes are rebuilt from resampled
returns: with `R` the actual returns `c_t / c_{t-1} - 1` for `t = s+1..e`, the first index is
`draw(|R|)`; after each, `draw(63) == 0` starts a new block at `draw(|R|)` and anything else moves to
the next index, wrapping at the end. `c'_s = c_s` and `c'_t = c'_{t-1} * (1 + R_j)`. Bar stamps are
unchanged.

**Report.** CAGR over `full` for each, the three percentiles of each, the share of paths where the
strategy's CAGR exceeds the benchmark's, and the share where the strategy's `max_drawdown_pct` is
below -90.

## 10. Gates and the grade

In this order. A gate that fails sets the grade if no earlier gate has; `not_applicable` neither
passes nor fails.

| gate | measured | passes when |
|---|---|---|
| `evidence` | `n_entries` (full), `n_entries_oos` | `>= 5` and `>= 2`; without a split, the second is not measured |
| `oos_benchmark` | the objective on `out_of_sample` (or `full`), strategy and benchmark | strategy better |
| `random_timing` | `strategy_percentile` | `>= 95` |
| `multiple_testing` | `p_value_corrected` | `< 0.05`; `not_applicable` without a sweep |
| `plateau` | `plateau_ratio` | `>= 0.5`; `not_applicable` without a sweep; a null ratio fails |
| `survival` | full-window `max_drawdown_pct`, share of paths below -90% | `> -90` and `< 0.05` |

`evidence` fails to `insufficient_evidence`, `oos_benchmark` to `not_supported`, and every other
gate to `fragile`. All pass: `supported`. The data grade is `exercise_only` unless every series used
is `research` tier.
