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

## Open questions

- Cap on sweep size (combinations across all params) before the engine refuses or samples.
- Multi-asset templates will need `position_pct` per asset and a rebalancing block.
- Whether `warmup_days` should default to "all available history", given how much it moves
  all-time-high-based results (see the TQQQ example).
