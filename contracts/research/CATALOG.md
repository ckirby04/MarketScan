# Data catalog

What data exists, how far back it goes, how good it is, and how to extend it. The catalog is
the only source the LLM may use for facts about data. It never states an inception date, an
expense ratio, a coverage range or a leverage factor from its own memory.

Record shapes are in [`catalog_records.schema.json`](catalog_records.schema.json). The LLM reaches
the catalog through the tools in [`assistant/tools.json`](assistant/tools.json).

---

## Inherited rules

The catalog sits on top of the MarketScan data layer and does not relax any of its rules.

- **A ticker is not an identity** ([`docs/IDENTITY.md`](../../docs/IDENTITY.md)). Securities are keyed by a
  permanent `security_id`. A ticker resolves to a security only with a date. The catalog's
  `resolve` tool takes `as_of`, and has no dateless form.
- **Quality is labelled, not implied** ([`docs/DATA.md`](../../docs/DATA.md)). Every series carries a
  `quality_tier`. A result computed on `exercise_only` data is labelled as such wherever it is
  shown.
- **Sources are used on their terms** ([`docs/DATA_POLICY.md`](../../docs/DATA_POLICY.md)). Every series records its
  source and licence, including whether its data may be shown to someone other than the operator.
  A series whose terms forbid redistribution can feed a private run but not a published one.

---

## Records

### Security

One company or fund over its whole life.

| field | meaning |
|---|---|
| `security_id` | Permanent and never reused. What every other record keys on. |
| `name` | Human-readable name. |
| `asset_class` | `equity`, `etf`, `index`, `rate`. |
| `ticker_history` | `[{ticker, from, to}]`. `to: null` means current. A rename adds a row; a reused ticker is a different security. |
| `status` | `active` or `delisted`. |
| `delisting` | Date, reason (`merger`, `bankruptcy`, `voluntary`, `other`), and whether the delisting return is known. Required when `status` is `delisted`. |
| `fund` | ETF only: `leverage` (3, -1, ...), `underlying_security_id`, `expense_ratio_pct` with its `as_of` date, `rebalance` (`daily` for leveraged and inverse funds). |

### Series

The bars actually held for a security. A security can have more than one series (two sources),
and the catalog nominates one as `primary`.

| field | meaning |
|---|---|
| `series_id`, `security_id` | |
| `source` | Name of the vendor or `synthetic:<recipe_id>`. |
| `interval` | `1d` only for now. |
| `first_bar`, `last_bar`, `bar_count` | Coverage. |
| `gaps` | Spans of missing sessions longer than one session, as `[{from, to}]`. |
| `adjustment` | `splits` and `dividends`: each `vendor`, `computed` or `none`. `total_return_available` is true only when dividends are present. |
| `quality_tier` | `research`: delisting coverage plus documented, point-in-time adjustments. `exercise_only`: anything less, which today means every free source. |
| `license` | `redistributable` (bool), `terms_url`, `notes`. |
| `primary` | Whether this is the default series for its security. |

### Synthetic recipe

A way to extend a security's history back before its first bar, by simulating it from an
underlying.

| field | meaning |
|---|---|
| `recipe_id` | |
| `target_security_id`, `underlying_security_id` | |
| `method` | `daily_leverage` only for now: `r = L * r_u - (fee + (L - 1) * (cash_rate + borrow_spread)) / 252`. |
| `params` | `leverage`, `annual_fee_pct`, `borrow_spread_pct`, `cash_rate_series_id`. |
| `available_from` | The earliest date the recipe can produce: the later of the underlying's and the cash rate's first bar. |
| `validation` | Measured over the period where both the real and the synthetic series exist: `overlap_from`, `overlap_to`, `daily_return_correlation`, `annualized_tracking_difference_pct` (synthetic minus real), `max_cumulative_gap_pct`. A recipe with no validation block cannot be used. |

Validation is what lets the insight step say how far to trust the simulated years, in numbers.
It is computed by a script from the two series, never typed in.

### Rate series

Cash and borrowing rates, such as the 3-month T-bill. A rate is a security with
`asset_class: rate` and an ordinary series record, so it carries the same coverage, quality and
licence fields as a price series. The form's `cash.yield: tbill_3m` resolves to one of these, and so does a
synthetic recipe's `cash_rate_series_id`.

### Regime

A named stress period, used by `robustness.regime_breakdown`.

| field | meaning |
|---|---|
| `regime_id` | e.g. `dotcom_bust`. |
| `name` | e.g. "Dot-com bust". |
| `from`, `to` | Dates. |
| `reference` | The security and rule the dates were taken from, e.g. QQQ peak-to-trough. |

Regimes are catalog data rather than hard-coded so that the dates are written down once and
the insight step and the engine agree on them.

---

## Questions the LLM asks the catalog

| tool | answers |
|---|---|
| `search_securities(query)` | "Which securities match this ticker or name?" Returns every match across time, including delisted and renamed ones, each with its ticker history. |
| `resolve(ticker, as_of)` | "Which security did this ticker mean on this date?" Returns one security or none. |
| `get_coverage(security_id)` | The primary series' coverage, gaps, adjustments, quality tier and licence, plus every synthetic recipe that targets the security. |
| `list_regimes(from, to)` | The regimes overlapping a date range. |

Resolution uses the form's start date as `as_of`. A ticker that resolves to a different security
at the start date than at the end date was renamed or reused within the range, and the LLM has to
say so rather than pick one.

---

## Coverage decisions

When a requested range starts before a security's `first_bar`, the LLM has three options, in
this order of preference:

1. **Synthetic extension**, when a validated recipe reaches back far enough. It fills
   `universe.assets[i].synthetic_extension` from the recipe and records `recipe_id`.
2. **Shortened range**, starting at `first_bar`. The provenance says why.
3. **Ask**, when neither is clearly right. For example, a recipe exists but its validation is
   weak (correlation below 0.98 or tracking difference above 2% a year), or the shortened range
   would drop a regime the question depends on.

Whichever happens, the user is told before the run, never only in the results.

---

## Open questions

- Where the catalog lives: a table in the MarketScan bar store, or a separate SQLite file
  generated from the `.msb` cache plus a hand-maintained fund and regime file.
- Expense ratios change over time. One `as_of` value is enough for now; a history may be needed
  if synthetic recipes are validated across a fee change.
- Intraday intervals are out of scope until there is a source for them.
