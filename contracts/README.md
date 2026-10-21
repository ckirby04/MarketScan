# Contracts

Machine-readable agreements that more than one part of the system loads at runtime.

These are not specs in the sense of [`docs/specs/`](../docs/specs/), which are historical records
and are never edited. A contract here is the current truth, and it changes when the system does.
Code validates against these files, not against a copy of them.

| | |
|---|---|
| [`research/`](research/) | The Research view: Strategy Form, data catalog records, Results, and the assistant's prompt and tools. Built from [`SPEC_RESEARCH_VIEW.md`](../docs/specs/SPEC_RESEARCH_VIEW.md). |

## Two validators, on purpose

Every schema here is checked by two independent implementations: the engine's
(`include/marketscan/research/json_schema.hpp`) and the research layer's (`research/contracts.py`,
which uses the `jsonschema` package). Both run over one labelled corpus,
`data/samples/research/schema_cases.json`, and each must give every case the expected verdict, so
they agree with each other through the labels.

Neither may be derived from the other. That is the same argument as
[`AUDIT.md`](../docs/AUDIT.md) "Deliberate duplication": two implementations agreeing is evidence
only while they are independent.

The engine's validator implements a **subset** of JSON Schema 2020-12 and refuses to compile a
schema that uses any keyword outside it. A keyword that was silently ignored would let every
document pass that rule, and nothing downstream could tell.
