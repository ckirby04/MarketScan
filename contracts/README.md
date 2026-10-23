# Contracts

Machine-readable agreements that more than one part of the system loads at runtime.

These are not specs in the sense of [`docs/specs/`](../docs/specs/), which are historical records
and are never edited. A contract here is the current truth, and it changes when the system does.
Code validates against these files, not against a copy of them.

| | |
|---|---|
| [`research/`](research/) | The Research view: Strategy Form, data catalog records, Results, the assistant's prompt and tools, and `plain_words.json`, the words both the web and desktop apps show for the Results' identifiers. Built from [`SPEC_RESEARCH_VIEW.md`](../docs/specs/SPEC_RESEARCH_VIEW.md). |

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

## Running a form

```powershell
ms_research --form form.json --daily data/cache/stooq --out runs/my-question
ms_research --form form.json --csv FIXTURE=prices.csv --out runs/fixture --trace
```

It validates the form against `strategy_form.schema.json`, runs the job
([`research/JOB.md`](research/JOB.md)), validates the Results against `results.schema.json` -- a
Results document that fails its own schema is reported as an engine bug, not written -- and writes
`form.json`, `results.json`, `equity.csv` and `curves.json`: the same run directory the Research view
writes, so a run made here appears in its Recent list. A form the job cannot honour is refused with every
reason, each naming the form path it is about.

`ms_research` is its own executable rather than `marketscan --headless --research`, as
[`SPEC_RESEARCH_VIEW.md`](../docs/specs/SPEC_RESEARCH_VIEW.md) part R3 had it, for the reason
`ms_live` is: what a binary can do should have a one-line answer.

The job's cross-test fixture, `data/samples/research/job_results.json` and `job_trace.json`, is
written by:

```powershell
ms_research --form data/samples/research/job_form.json `
    --csv FIXTURE=data/samples/research/drawdown_prices.csv --out <dir> `
    --trace --null-draws 1000 --paths 50 --fixed-time --data-snapshot fixture
```

and copied in. Regenerate it only on purpose: `tests/test_research_job.cpp` fails until the engine
reproduces it, and `research/tests/test_grading.py` then says whether the research layer agrees.
