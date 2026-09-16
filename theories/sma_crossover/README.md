# sma_crossover

A fast simple moving average crossing a slow one. Long when the fast average is
above the slow one, flat (or short, optionally) when it is below.

> **This is a calibration instrument, not a strategy.**
>
> **On realistic fills it should come out flat to modestly negative after costs.
> If it backtests profitably, the fill simulator is wrong.**

---

## What it is for

A moving-average crossover is the oldest technical signal there is, and on any
reasonably efficient instrument it has no edge. What it does have is
**turnover**: on noisy intraday data the two averages cross constantly, so it
trades often, and every trade pays the spread and a commission.

That makes it a measuring instrument. Point it at a dataset, run it under both
fill models, and subtract. The gap is the cost of the spread multiplied by how
often the strategy trades — the number a backtest built on optimistic fills
silently awards itself.

On the committed sample (two sessions of one-minute SPY-like bars, defaults of
fast 10 / slow 40):

```
                          equity   return   sharpe   max dd  trades   slippage
spread-crossing fills   98,647.11   -1.35%   -17.13    2.25%      20   1,084.26
optimistic fills        99,729.37   -0.27%    -3.51    1.38%      20       0.00

                                                    imaginary profit: 1,082.26
```

Twenty round trips turned roughly **1.1% of capital** into the spread. The
benchmark, which trades once, gave up $56. That ratio — not either number on
its own — is what this theory is here to show.

## Reading the result

| If you see | It means |
|---|---|
| Flat to modestly negative on realistic fills | Working as intended. |
| **Profitable on realistic fills** | The fill simulator is wrong. Check that quotes exist in the data (bar-only files need `synthesize_quotes`), that market orders are crossing the spread, and that commission is being charged. |
| Identical under both fill models | No spread is being crossed. The feed is probably supplying bars with no quotes. |
| Zero trades | The averages never crossed — the data is too short, or the slow period is longer than the dataset. |

## Parameters

| Name | Default | Notes |
|---|---|---|
| `fast` | 10 | Fast average length, in bars. |
| `slow` | 40 | Slow average length. Must exceed `fast`, or the theory stands aside rather than inventing a signal. |
| `symbol_index` | 0 | Which instrument to trade. Tickers cannot be parameters — every `MS_ParamDesc` is a double. |
| `cash_reserve_pct` | 2 | Held back so crossing the spread cannot overdraw the account. This is a cash account: an order it cannot pay for is refused whole. |
| `allow_shorts` | false | Take the short side on a downward cross instead of standing aside. **Borrow cost is not modelled**, so a short here is cheaper than a real one. |

The averages are recomputed from the history buffer on every bar rather than
carried as running sums. That is deliberate: the periods are live parameters the
GUI can move mid-run, and a running sum would then be a sum over a window that
no longer exists.

## What it does *not* do

No position sizing beyond "spend the available cash", no stops, no filter on
trend strength, no attempt to trade less when the averages are close together.
Each of those would make it better, and each would make it less useful as a
measuring instrument — the point is that it trades a lot and pays for it.
