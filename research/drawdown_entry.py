"""The drawdown_entry rule: the research layer's implementation.

Specification: contracts/research/DRAWDOWN_ENTRY.md. The engine's implementation
is src/marketscan/research/drawdown_entry.cpp, and this one was written from the
specification, not from that file -- see docs/AUDIT.md "Deliberate duplication".
They are cross-tested on data/samples/research/drawdown_cases.json for exact
agreement on every fill.

Each block cites the step it implements.
"""

from __future__ import annotations

import datetime as dt
import math
from dataclasses import dataclass, field

MICROS = 1_000_000

BUY_TAG = 0x100
SELL_TAG = 0x200

# Step 3's fixed order, and the spec's reason codes.
RECOVER_TO_HIGH, TAKE_PROFIT, STOP_LOSS, TRAILING_STOP, TIME_STOP = 1, 2, 3, 4, 5


def llround(x: float) -> int:
    """Half away from zero, on the double as given.

    Not floor(x + 0.5): that adds before rounding, and 0.49999999999999994 + 0.5
    is 1.0 in double precision.
    """
    a = abs(x)
    whole = math.floor(a)
    r = whole + 1 if a - whole >= 0.5 else whole
    return int(r) if x >= 0 else -int(r)


@dataclass(frozen=True)
class Params:
    reference: str = "all_time_high"  # | "rolling_high" | "since_last_exit"
    window: int = 252
    tranches: tuple[tuple[float, float], ...] = ((20.0, 100.0),)  # (level, fraction)
    recover_to_high: bool = True
    take_profit: float = 0.0
    stop_loss: float = 0.0
    trailing_stop: float = 0.0
    time_stop: int = 0
    reentry: bool = True


@dataclass(frozen=True)
class Costs:
    starting_cash: int  # micro-dollars
    spread_bps: float
    commission: float  # dollars per order
    reserve: float  # percent


@dataclass
class Fill:
    bar: int
    side: str  # "buy" | "sell"
    quantity: float
    price: float
    tag: int


@dataclass
class Result:
    fills: list[Fill] = field(default_factory=list)
    rejected: int = 0


def day_number(date: dt.date) -> int:
    return (date - dt.date(1970, 1, 1)).days


def simulate(
    dates: list[dt.date], closes: list[float], params: Params, costs: Costs, start_day: int
) -> Result:
    out = Result()
    commission = llround(costs.commission * MICROS)
    half_fraction = costs.spread_bps * 1e-4 * 0.5

    cash = costs.starting_cash
    held = 0.0

    # Running quantities, step 1.
    all_time_high = -math.inf
    since_exit_high = -math.inf
    exited_once = False

    # The campaign.
    active = False
    base = 0
    entry_high = 0.0
    filled = 0  # tranche mask
    first_fill_bar = -1
    cost_sum = 0.0
    qty_sum = 0.0
    peak = -math.inf

    pending = None  # (side, quantity, tag), decided on the previous bar

    def end_campaign():
        nonlocal active, filled, first_fill_bar, cost_sum, qty_sum, peak
        active = False
        filled = 0
        first_fill_bar = -1
        cost_sum = 0.0
        qty_sum = 0.0
        peak = -math.inf

    for t, c in enumerate(closes):
        # --- The fill, on this bar's quote (execution model) -----------------
        if pending is not None:
            side, q, tag = pending
            pending = None
            half = c * half_fraction
            if side == "buy":
                ask = c + half
                price = max((ask * q) / q, ask)
                debit = llround(price * q * MICROS) + commission
                if debit > cash:
                    out.rejected += 1
                    # Step 7: the tranches stay unfilled; nothing held means
                    # the campaign has ended, without an exit.
                    if held <= 0.0:
                        end_campaign()
                else:
                    cash -= debit
                    held += q
                    if first_fill_bar < 0:
                        first_fill_bar = t
                        peak = -math.inf
                    filled |= tag & 0xFF
                    cost_sum += price * q
                    qty_sum += q
                    out.fills.append(Fill(t, "buy", q, price, tag))
            else:
                bid = c - half
                price = min((bid * q) / q, bid)
                cash += llround(price * q * MICROS) - commission
                held -= q
                out.fills.append(Fill(t, "sell", q, price, tag))
                end_campaign()
                exited_once = True
                since_exit_high = -math.inf

        # --- Steps 1 and 2 ---------------------------------------------------
        all_time_high = max(all_time_high, c)
        since_exit_high = max(since_exit_high, c)
        if first_fill_bar >= 0:
            peak = max(peak, c)
        if params.reference == "rolling_high":
            high = max(closes[max(0, t + 1 - max(params.window, 1)) : t + 1])
        elif params.reference == "since_last_exit" and exited_once:
            high = since_exit_high
        else:
            high = all_time_high
        drawdown = max(0.0, 100.0 * (1.0 - c / high))

        if day_number(dates[t]) < start_day:
            continue

        # --- Step 3 ----------------------------------------------------------
        if held > 0.0:
            avg = cost_sum / qty_sum
            reason = 0
            if params.recover_to_high and c >= entry_high:
                reason = RECOVER_TO_HIGH
            elif params.take_profit > 0.0 and c >= avg * (1.0 + params.take_profit / 100.0):
                reason = TAKE_PROFIT
            elif params.stop_loss > 0.0 and c <= avg * (1.0 - params.stop_loss / 100.0):
                reason = STOP_LOSS
            elif params.trailing_stop > 0.0 and c <= peak * (1.0 - params.trailing_stop / 100.0):
                reason = TRAILING_STOP
            elif params.time_stop > 0 and t - first_fill_bar >= params.time_stop:
                reason = TIME_STOP
            if reason:
                pending = ("sell", held, SELL_TAG | reason)
                continue

        # --- Step 4 ----------------------------------------------------------
        if exited_once and not params.reentry:
            continue
        mask = 0
        fraction = 0.0
        for k, (level, frac) in enumerate(params.tranches):
            if not filled & (1 << k) and drawdown >= level:
                mask |= 1 << k
                fraction += frac
        if not mask:
            continue

        # --- Step 5: flat at a campaign's start, so equity is cash -----------
        starting = not active
        if starting:
            active = True
            base = cash
            entry_high = high

        # --- Step 6 ----------------------------------------------------------
        wanted = llround(base * fraction / 100.0)
        budget = min(wanted, cash - commission)
        estimate = c * (1.0 + costs.reserve / 100.0)
        shares = math.floor((budget / MICROS) / estimate)
        if not shares >= 1:
            if starting:
                active = False
            continue

        # --- Step 7 ----------------------------------------------------------
        pending = ("buy", float(shares), BUY_TAG | mask)

    return out


def params_from_case(case: dict) -> Params:
    return Params(
        reference=case.get("reference", "all_time_high"),
        window=case.get("window", 252),
        tranches=tuple((float(a), float(b)) for a, b in case["tranches"]),
        recover_to_high=case.get("recover_to_high", False),
        take_profit=case.get("take_profit", 0.0),
        stop_loss=case.get("stop_loss", 0.0),
        trailing_stop=case.get("trailing_stop", 0.0),
        time_stop=case.get("time_stop", 0),
        reentry=case.get("reentry", True),
    )
