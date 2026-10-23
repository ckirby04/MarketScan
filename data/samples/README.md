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
- `job_form.json`, a Strategy Form over that series, with `job_results.json` and `job_trace.json`,
  the Results and trace the engine's research job produced for it. The engine must reproduce both
  exactly, and the research layer must regrade the trace to the same Results; see
  `contracts/README.md` for the command that writes them.
- `chat/`, one conversation with the job service's assistant, recorded from a service running
  locally: each turn's NDJSON exactly as streamed (a question card, the answer and the ready
  form, the analysis), the conversation as the service showed it after each step, the run's
  response and its polled statuses. The desktop chat's tests replay them through a fake transport
  (`tests/test_research_chat.cpp`), so the client is tested against what the service actually
  sends. Model text and form fields only; no prices.
