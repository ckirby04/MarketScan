# Sample datasets

Small, synthetic, and committed on purpose: the test suite and the first run of the application
need data that is present in a fresh clone, and a repository is not a place for market data.

Nothing in here is real market data. Nothing in here came from a vendor. See `.gitignore` —
everything under `data/` except this directory is ignored.

## `research/`

Fixtures for the Research view (`contracts/research/`):

- `schema_cases.json`, the labelled corpus both schema validators must agree with, and
  `results_fixture.json`, a structurally valid Results document whose numbers mean nothing. See
  `contracts/README.md`.
- `drawdown_prices.csv`, a seeded synthetic close series built to reach every branch of the
  `drawdown_entry` rule, with `drawdown_cases.json` and `drawdown_expected.json`, the fills both
  implementations must reproduce. All three are written by `scripts/make_drawdown_fixture.py`.
