"""What a position is worth after the last quoted price. The assumed exit, named.

Session D Part 2. `docs/EXECUTION.md` calls the shared limitation **the assumed
exit**: neither layer of this project knows what a delisting actually cost, so
both substitute something. This module is the research layer's half of the
switch, and `include/marketscan/exec/delisting.hpp` is the engine's. The setting
names are identical on both sides on purpose -- when CRSP delisting returns
arrive, turning this on is the whole of the change.

THE TWO SETTINGS

`last_price` liquidates at the final quoted price and applies no delisting
return. **This is the default, and it is the optimistic one.** It is what a
backtest does when nobody has thought about the question, and it is what the
engine has always done.

`apply_true_return` uses the delisting return where the data supplies it. On a
synthetic panel the generator supplies it by construction; on real data only a
vendor who measured it can.

WHY THE DEFAULT IS THE WRONG-BUT-CURRENT ONE

Because changing a default silently moves every committed number that was
measured under it. The flattery this assumption buys is worth measuring, not
worth assuming away by fiat -- `research/pathology.py` runs the same strategy
under both settings and reports the difference, which is the number that says
how much a zero delisting return is worth to a backtest that makes one.
"""

from __future__ import annotations

from typing import Literal

import pandas as pd

DelistingReturnModel = Literal["last_price", "apply_true_return"]

LAST_PRICE: DelistingReturnModel = "last_price"
APPLY_TRUE_RETURN: DelistingReturnModel = "apply_true_return"

DEFAULT_MODEL: DelistingReturnModel = LAST_PRICE
"""The current behaviour, kept as the default so that no committed result moves
when this switch appears. See the module docstring."""

MODELS: tuple[DelistingReturnModel, ...] = (LAST_PRICE, APPLY_TRUE_RETURN)


def applied_returns(delistings: pd.DataFrame | None,
                    model: DelistingReturnModel = DEFAULT_MODEL) -> dict[str, float]:
    """The delisting return each consumer should apply, by security.

    Empty under `last_price`, which is the point: a consumer does not branch on
    the model, it applies whatever this returns. One place decides, and a caller
    that forgets to pass the model gets the conservative-for-committed-results
    answer rather than a silent behaviour change.
    """
    if model not in MODELS:
        raise ValueError(f"unknown delisting return model {model!r}; have {MODELS}")
    if model == LAST_PRICE or delistings is None or delistings.empty:
        return {}
    if "delisting_return" not in delistings.columns:
        raise ValueError("delistings frame carries no delisting_return column")
    return {str(security): float(value)
            for security, value in delistings["delisting_return"].items()}


def describe(model: DelistingReturnModel) -> str:
    """One line, for a report that has to say which assumption produced it."""
    if model == LAST_PRICE:
        return "last_price (liquidated at the final quoted price; no delisting return)"
    return "apply_true_return (the delisting return applied where the data supplies it)"
