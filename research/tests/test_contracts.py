"""The research contracts, from the research layer's side.

The engine's half is tests/test_research_contracts.cpp. Both reproduce the
verdicts in data/samples/research/schema_cases.json; see contracts/README.md.
"""

from __future__ import annotations

import json

import pytest

from research import contracts

CASES = contracts.load_cases()


@pytest.mark.parametrize("name", sorted(contracts.SCHEMA_FILES))
def test_every_contract_schema_compiles_inside_the_shared_subset(name):
    contracts.load_validator(name)


def test_the_assistant_tool_schemas_compile_inside_the_shared_subset():
    tools = json.loads(
        (contracts.CONTRACTS / "assistant" / "tools.json").read_text(encoding="utf-8")
    )
    assert len(tools) >= 1
    for tool in tools:
        contracts.validator(tool["input_schema"])


def test_the_corpus_exercises_every_schema_both_ways():
    # A corpus with no invalid cases would pass a validator that accepts
    # everything, and one with no valid cases would pass one that accepts
    # nothing.
    for name in contracts.SCHEMA_FILES:
        verdicts = {c["valid"] for c in CASES if c["schema"] == name}
        assert verdicts == {True, False}, name


def test_case_names_are_unique():
    names = [c["name"] for c in CASES]
    assert len(names) == len(set(names))


@pytest.mark.parametrize("case", CASES, ids=[c["name"] for c in CASES])
def test_case(case):
    errors = list(contracts.load_validator(case["schema"]).iter_errors(case["document"]))
    assert (not errors) == case["valid"], [e.message for e in errors]


def test_a_keyword_outside_the_subset_is_refused():
    with pytest.raises(ValueError, match="minLength"):
        contracts.validator({"type": "string", "minLength": 1})


def test_a_format_outside_the_subset_is_refused():
    with pytest.raises(ValueError, match="email"):
        contracts.validator({"type": "string", "format": "email"})


def test_the_format_checks_are_live_rather_than_annotations():
    # The vacuous-pass guard: if the format checker were not wired, both of these
    # would be valid.
    v = contracts.validator({"type": "string", "format": "date"})
    assert v.is_valid("2024-02-29")
    assert not v.is_valid("2023-02-29")
    v = contracts.validator({"type": "string", "format": "date-time"})
    assert v.is_valid("2026-10-21T00:00:00Z")
    assert not v.is_valid("2026-10-21T00:00:00")


@pytest.mark.parametrize(
    "text, ok",
    [
        ("2024-02-29", True),
        ("1900-02-29", False),
        ("2000-02-29", True),
        ("0001-01-01", True),
        ("0000-01-01", False),
        ("2026-13-01", False),
        ("2026-1-01", False),
        (" 2026-01-01", False),
        ("2026-01-01\n", False),
    ],
)
def test_is_date(text, ok):
    assert contracts.is_date(text) is ok


@pytest.mark.parametrize(
    "text, ok",
    [
        ("2026-10-21T00:00:00Z", True),
        ("2026-10-21T23:59:59.999999-04:00", True),
        ("2026-10-21t00:00:00Z", False),
        ("2026-10-21T00:00:00z", False),
        ("2026-10-21T00:00:60Z", False),
        ("2026-10-21T00:00:00+24:00", False),
        ("2026-10-21T00:00:00+00:60", False),
        ("2026-10-21T00:00Z", False),
    ],
)
def test_is_date_time(text, ok):
    assert contracts.is_date_time(text) is ok


def test_patch_refuses_to_replace_what_is_not_there():
    # A case whose patch silently added a key instead of replacing one would
    # test a different document from the one its name describes.
    with pytest.raises(KeyError):
        contracts.apply_patch({"a": 1}, [{"op": "replace", "path": "/b", "value": 2}])
