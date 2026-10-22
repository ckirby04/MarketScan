# Strategy Form schema (v0.1)

`strategy_form.schema.json` is the contract between the LLM, the UI and the backtest engine.
The engine accepts only forms that validate against it.

## Lifecycle

1. **Draft.** The LLM reads the prompt, picks a `template`, and fills every field it can.
   Values it cannot determine are `null` with a `provenance` entry of `source: "missing"`
   and the `options` to offer. Drafts are not expected to validate.
2. **Resolve.** Missing fields are asked, as buttons built from `options` where possible. Fields
   annotated `x-ask: "always"` are asked even if the LLM has a guess, unless the user stated them.
   Inferred and default values are shown in the form, flagged, and editable.
3. **Validate.** The resolved form must pass the schema with no `missing` provenance left.
   Validation errors go back to the LLM to fix, not to the user.
4. **Run.** The engine ignores `provenance` and `prompt`.
5. **Explain.** The insight step receives the form, the provenance and the results, so it can
   point out which assumptions were defaults and which history was synthetic.

## Conventions

- Percentages are positive numbers in percent: `threshold_pct: 40` means 40% below the reference high.
- Any `param` can be a number, `{ "sweep": {min, max, step} }` or `{ "values": [...] }`.
  A sweep makes the run an optimization ranked by `objective.metric`.
- Signals use a bar's close; fills happen on a later bar (`execution.fill`). Same-bar fills are
  not expressible on purpose.
- Provenance keys are JSON Pointers into the form.

## Adding a template

Add the name to `template`'s enum, define it under `$defs` with a `template` const, and add a
`$ref` to it in `strategy.oneOf`. Shared sections (universe, costs, objective, robustness) do not change.

## What the job can run today

The schema describes the forms the system is meant to accept; the job honours a subset, and
refuses the rest by path rather than approximating it ([`JOB.md`](JOB.md) section 1). Every
`x-default` is a value the job can honour, so a form that leaves those fields alone runs. The
committed example, `examples/tqqq_drawdown.json`, is the *target* shape -- synthetic history,
total return, a T-bill cash yield, next-open fills -- and is refused today on each of those
counts. `examples/starter_form.json` is one that runs.

The sweep is capped at 500 points (JOB.md section 2).

## Open questions

- Multi-asset templates will need `position_pct` per asset and a rebalancing block.
- Whether `warmup_days` should default to "all available history", given how much it moves
  all-time-high-based results (see the TQQQ example).
