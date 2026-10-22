"""Synthetic history for a leveraged fund, and how well it tracks the real one.

CATALOG.md's `daily_leverage` recipe, written out the way JOB.md section 1a fixes
it. This is the research layer's implementation; the engine's is in
src/marketscan/research/job.cpp, and data/samples/research/synthetic_cases.json
holds them to each other exactly. Neither derives from the other.

For an underlying bar k (k > 0), with `rate` the cash rate in percent a year
observed on or before the day of bar k - 1:

    cost = (fee + (L - 1) * (rate + spread)) / 100 / 252
    r_k  = L * (U_k / U_(k-1) - 1) - cost

and the synthetic close is built backwards from the target's first real close:
S at the underlying's bar on that day is the real close, and

    S_(k-1) = S_k / (1 + r_k)

for every earlier underlying bar that has a rate. Each step is a separate
floating-point operation, in this order; nothing is fused or reassociated.

Validation (`validate`) runs the same returns forwards over the years where the
real fund exists, and compares them with the fund's own: the daily-return
correlation, the annualized tracking difference (synthetic minus real), and the
largest gap between the two cumulative paths.
"""

from __future__ import annotations

import bisect
import math
from dataclasses import dataclass

Days = list[str]  # ISO dates, ascending


@dataclass(frozen=True)
class Params:
    leverage: float
    annual_fee_pct: float
    borrow_spread_pct: float


class RecipeError(ValueError):
    pass


def rate_on_or_before(days: Days, values: list[float], day: str) -> float | None:
    i = bisect.bisect_right(days, day) - 1
    return values[i] if i >= 0 else None


def daily_return(u_prev: float, u: float, rate: float, p: Params) -> float:
    cost = (p.annual_fee_pct + (p.leverage - 1.0) * (rate + p.borrow_spread_pct)) / 100.0 / 252.0
    return p.leverage * (u / u_prev - 1.0) - cost


def extend(underlying_days: Days, underlying: list[float],
           rate_days: Days, rates: list[float],
           first_real_day: str, first_real_close: float,
           p: Params) -> tuple[Days, list[float]]:
    """The synthetic closes before `first_real_day`, oldest first.

    The underlying must have a bar on `first_real_day`. Synthetic bars go back
    to the earliest underlying bar with a rate on or before its day.
    """
    try:
        anchor = underlying_days.index(first_real_day)
    except ValueError:
        raise RecipeError(f"the underlying has no bar on {first_real_day}, where the real history starts") from None
    days: Days = []
    closes: list[float] = []
    s = first_real_close
    for k in range(anchor, 0, -1):
        rate = rate_on_or_before(rate_days, rates, underlying_days[k - 1])
        if rate is None:
            break
        r = daily_return(underlying[k - 1], underlying[k], rate, p)
        if not 1.0 + r > 0.0:
            raise RecipeError(f"the recipe takes the price to zero on {underlying_days[k]}")
        s = s / (1.0 + r)
        days.append(underlying_days[k - 1])
        closes.append(s)
    days.reverse()
    closes.reverse()
    return days, closes


def available_from(underlying_days: Days, rate_days: Days) -> str | None:
    """The earliest day the recipe can produce: the first underlying bar with a rate."""
    if not rate_days:
        return None
    i = bisect.bisect_left(underlying_days, rate_days[0])
    return underlying_days[i] if i < len(underlying_days) else None


def validate(underlying_days: Days, underlying: list[float],
             rate_days: Days, rates: list[float],
             target_days: Days, target: list[float], p: Params) -> dict:
    """CATALOG.md's validation block, over the days both series have."""
    u_index = {d: i for i, d in enumerate(underlying_days)}
    syn: list[float] = []
    real: list[float] = []
    pairs_from = pairs_to = None
    for j in range(1, len(target_days)):
        a, b = target_days[j - 1], target_days[j]
        ia, ib = u_index.get(a), u_index.get(b)
        # Only where the two series step over the same interval: the next
        # underlying bar after a is b.
        if ia is None or ib is None or ib != ia + 1:
            continue
        rate = rate_on_or_before(rate_days, rates, a)
        if rate is None:
            continue
        syn.append(daily_return(underlying[ia], underlying[ib], rate, p))
        real.append(target[j] / target[j - 1] - 1.0)
        pairs_from = pairs_from or a
        pairs_to = b
    if len(syn) < 2:
        raise RecipeError("the real and synthetic series do not overlap")

    n = len(syn)
    mean_s = sum(syn) / n
    mean_r = sum(real) / n
    cov = sum((x - mean_s) * (y - mean_r) for x, y in zip(syn, real))
    var_s = sum((x - mean_s) ** 2 for x in syn)
    var_r = sum((y - mean_r) ** 2 for y in real)
    correlation = cov / math.sqrt(var_s * var_r)

    growth_s = growth_r = 1.0
    gap = 0.0
    for x, y in zip(syn, real):
        growth_s *= 1.0 + x
        growth_r *= 1.0 + y
        gap = max(gap, abs(growth_s / growth_r - 1.0))
    tracking = (growth_s ** (252.0 / n) - growth_r ** (252.0 / n)) * 100.0
    return {
        "overlap_from": pairs_from,
        "overlap_to": pairs_to,
        "daily_return_correlation": correlation,
        "annualized_tracking_difference_pct": tracking,
        "max_cumulative_gap_pct": gap * 100.0,
    }
