# Sample datasets

Small, synthetic, and committed on purpose: the test suite and the first run of the application
need data that is present in a fresh clone, and a repository is not a place for market data.

Nothing in here is real market data. Nothing in here came from a vendor. See `.gitignore` —
everything under `data/` except this directory is ignored.

## `research/`

JSON fixtures for the Research view's contracts (`contracts/research/`), not price data:
`schema_cases.json`, the labelled corpus both schema validators must agree with, and
`results_fixture.json`, a structurally valid Results document whose numbers mean nothing. See
`contracts/README.md`.
