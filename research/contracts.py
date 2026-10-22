"""The research contracts, validated by the research layer.

One of two validators for `contracts/research/`. The other is the engine's,
`include/marketscan/research/json_schema.hpp`. Both are run over
`data/samples/research/schema_cases.json` and must give every case its expected
verdict. This one is built on the `jsonschema` package and shares nothing with
the engine's but the schemas and the corpus -- see contracts/README.md for why
that independence is the point.

TWO THINGS THE PACKAGE DOES NOT DO BY DEFAULT, DONE HERE

`format` is an annotation in JSON Schema 2020-12 unless a format checker is
supplied, and the package's own `date-time` checker silently does nothing when
an optional dependency is missing. A validator that quietly skipped every date
would pass the whole corpus's valid cases and fail none of the invalid ones for
the right reason. So the two formats the contracts use are checked by functions
written here, against the rule in contracts/README.md, and the package's
checker is not used at all.

The engine's validator refuses any keyword outside a closed subset. The package
accepts all of 2020-12, so `check_vocabulary` holds the contracts to the same
subset from this side. A contract that started using `patternProperties` would
otherwise pass here and fail to compile in the engine, and the disagreement
would surface as a mystery rather than as a refusal naming the keyword.
"""

from __future__ import annotations

import calendar
import copy
import json
import re
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import jsonschema

REPO = Path(__file__).resolve().parent.parent
CONTRACTS = REPO / "contracts" / "research"
CASES = REPO / "data" / "samples" / "research" / "schema_cases.json"

SCHEMA_FILES = {
    "form": CONTRACTS / "strategy_form.schema.json",
    "results": CONTRACTS / "results.schema.json",
    "catalog": CONTRACTS / "catalog_records.schema.json",
}

# The subset both validators implement. contracts/README.md.
APPLICATORS = {
    "$ref", "$defs", "properties", "additionalProperties", "items",
    "allOf", "anyOf", "oneOf", "if", "then", "else",
}
ASSERTIONS = {
    "type", "enum", "const", "required", "minimum", "maximum",
    "exclusiveMinimum", "exclusiveMaximum", "minItems", "maxItems",
    "pattern", "format",
}
ANNOTATIONS = {"$schema", "$id", "$comment", "title", "description", "default", "examples"}
FORMATS = {"date", "date-time"}

_DATE = re.compile(r"([0-9]{4})-([0-9]{2})-([0-9]{2})", re.ASCII)
_TIME = re.compile(
    r"([0-9]{2}):([0-9]{2}):([0-9]{2})(\.[0-9]+)?(Z|[+-]([0-9]{2}):([0-9]{2}))",
    re.ASCII,
)


def is_date(text: str) -> bool:
    """YYYY-MM-DD, a real calendar date, year 1 or later."""
    match = _DATE.fullmatch(text)
    if match is None:
        return False
    year, month, day = (int(g) for g in match.groups())
    if year < 1 or not 1 <= month <= 12:
        return False
    return 1 <= day <= calendar.monthrange(year, month)[1]


def is_date_time(text: str) -> bool:
    """RFC 3339 with an upper-case T and Z, seconds 00-59 (no leap second)."""
    date, sep, clock = text.partition("T")
    if not sep or not is_date(date):
        return False
    match = _TIME.fullmatch(clock)
    if match is None:
        return False
    hour, minute, second = int(match[1]), int(match[2]), int(match[3])
    if hour > 23 or minute > 59 or second > 59:
        return False
    if match[5] != "Z" and (int(match[6]) > 23 or int(match[7]) > 59):
        return False
    return True


def _format_checker() -> jsonschema.FormatChecker:
    checker = jsonschema.FormatChecker(formats=())
    # A non-string is not this checker's business: `type` reports it.
    checker.checks("date")(lambda v: not isinstance(v, str) or is_date(v))
    checker.checks("date-time")(lambda v: not isinstance(v, str) or is_date_time(v))
    return checker


def _subschemas(node: Any, path: str) -> Iterator[tuple[str, dict]]:
    """Every schema object reachable through an applicator, with its path."""
    if isinstance(node, bool):
        return
    yield path, node
    for key in ("$defs", "properties"):
        for name, sub in node.get(key, {}).items():
            yield from _subschemas(sub, f"{path}/{key}/{name}")
    for key in ("additionalProperties", "items", "if", "then", "else"):
        if key in node:
            yield from _subschemas(node[key], f"{path}/{key}")
    for key in ("allOf", "anyOf", "oneOf"):
        for i, sub in enumerate(node.get(key, [])):
            yield from _subschemas(sub, f"{path}/{key}/{i}")


def check_vocabulary(schema: dict) -> list[str]:
    """Keywords outside the shared subset, as 'path: keyword' strings."""
    allowed = APPLICATORS | ASSERTIONS | ANNOTATIONS
    problems = []
    for path, node in _subschemas(schema, ""):
        for key in node:
            if key not in allowed and not key.startswith("x-"):
                problems.append(f"{path or '/'}: {key}")
        if "format" in node and node["format"] not in FORMATS:
            problems.append(f"{path or '/'}: format {node['format']!r}")
    return problems


def validator(schema: dict) -> jsonschema.Draft202012Validator:
    jsonschema.Draft202012Validator.check_schema(schema)
    problems = check_vocabulary(schema)
    if problems:
        raise ValueError("outside the shared subset: " + "; ".join(problems))
    return jsonschema.Draft202012Validator(schema, format_checker=_format_checker())


def load_validator(name: str) -> jsonschema.Draft202012Validator:
    return validator(json.loads(SCHEMA_FILES[name].read_text(encoding="utf-8")))


# --- The corpus ---------------------------------------------------------------


def _pointer_tokens(pointer: str) -> list[str]:
    if pointer == "":
        return []
    if not pointer.startswith("/"):
        raise ValueError(f"not a JSON Pointer: {pointer!r}")
    return [t.replace("~1", "/").replace("~0", "~") for t in pointer[1:].split("/")]


def apply_patch(document: Any, patch: list[dict]) -> Any:
    """RFC 6902 add, replace and remove. The corpus uses nothing else."""
    doc = copy.deepcopy(document)
    for op in patch:
        *parents, last = _pointer_tokens(op["path"])
        target = doc
        for token in parents:
            target = target[int(token)] if isinstance(target, list) else target[token]
        kind = op["op"]
        if isinstance(target, list):
            index = len(target) if last == "-" else int(last)
            if kind == "add":
                target.insert(index, op["value"])
            elif kind == "replace":
                target[index] = op["value"]
            elif kind == "remove":
                del target[index]
            else:
                raise ValueError(f"unsupported op {kind!r}")
        else:
            if kind in ("replace", "remove") and last not in target:
                raise KeyError(f"{op['path']} does not exist")
            if kind in ("add", "replace"):
                target[last] = op["value"]
            elif kind == "remove":
                del target[last]
            else:
                raise ValueError(f"unsupported op {kind!r}")
    return doc


def load_cases() -> list[dict]:
    """Each case with its document materialised."""
    corpus = json.loads(CASES.read_text(encoding="utf-8"))
    bases = {
        name: json.loads((REPO / path).read_text(encoding="utf-8"))
        for name, path in corpus["bases"].items()
    }
    cases = []
    for case in corpus["cases"]:
        if "document" in case:
            document = case["document"]
        else:
            document = apply_patch(bases[case["base"]], case["patch"])
        cases.append({**case, "document": document})
    return cases
