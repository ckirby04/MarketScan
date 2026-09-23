"""The duplication that is the instrument, enforced rather than only documented.

WHAT THIS GUARDS

`docs/AUDIT.md` under "Deliberate duplication" argues that two implementations
agreeing is evidence only while they are causally independent. `research/sizing.py`
and `research/parity.py` are the Python halves of two such pairs; the C++ halves
are `include/marketscan/strategy/sizing.hpp` and `theories/decile_parity/`. The
tests that compare them assert exact equality, with no tolerance.

Which means those tests cannot notice the one change that would destroy them. If
either Python half started calling the C++ engine through the pybind11 bindings,
the comparison would still pass -- perfectly, every time -- because one
implementation always agrees with itself. Every printed number would survive. The
measurement would be gone.

Session G wrote that argument down and said plainly that nothing mechanical
enforced it. This is the mechanism.

WHAT IT DOES NOT GUARD

The bindings are not banned from `research/`, or from anywhere else. They exist
for good reasons and are used legitimately -- `scripts/make_quickstart_notebook.py`
and `bindings/python/examples/smoke_test.py` both import them, correctly. Only the
modules whose independence *is* a measurement are constrained, and they are named
one by one below rather than matched by a pattern, because a pattern would quietly
grow to cover modules nobody decided about.

WHY STATIC ANALYSIS RATHER THAN A RUNTIME IMPORT CHECK

The `research` CI job runs on Linux with no C++ toolchain and no built bindings,
deliberately: the research layer depends on nothing in the C++ build and the first
import that changes that turns the job red. A runtime check would need the
bindings present to be meaningful, so it would be vacuous exactly where this job
runs. Reading the import graph works either way.
"""

from __future__ import annotations

import ast
import pathlib
import textwrap

import pytest

# The pybind11 module. `bindings/python/module.cpp` declares
# PYBIND11_MODULE(marketscan, m).
BINDINGS = "marketscan"

REPO = pathlib.Path(__file__).resolve().parents[2]

# The modules whose independence from the C++ engine is itself the measurement.
# Named individually on purpose. Adding one is a decision; matching a prefix is
# not.
GUARDED = {
    "research.sizing": (
        "the sizing rule, cross-tested bit-for-bit against "
        "include/marketscan/strategy/sizing.hpp"
    ),
    "research.parity": (
        "the decile strategy reference implementation, cross-tested to the "
        "micro-dollar against theories/decile_parity"
    ),
}


def module_path(root: pathlib.Path, module: str) -> pathlib.Path | None:
    """The file backing a dotted first-party module name, if there is one."""
    relative = pathlib.Path(*module.split("."))
    for candidate in (root / relative.with_suffix(".py"), root / relative / "__init__.py"):
        if candidate.is_file():
            return candidate
    return None


def direct_imports(path: pathlib.Path, root: pathlib.Path) -> set[str]:
    """Every module name this file imports, as written.

    A relative import is resolved against the file's own package so that
    `from . import sizing` is followed rather than skipped -- an unfollowed edge
    is a hole in the guard. `root` is threaded through rather than taken from a
    module constant so the synthetic cases below can be analysed in a tmp_path:
    a guard that can only run against one directory cannot be tested.
    """
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))

    parts = path.relative_to(root).with_suffix("").parts
    package = parts[:-1] if parts[-1] != "__init__" else parts

    out: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                out.add(alias.name)
        elif isinstance(node, ast.ImportFrom):
            if node.level == 0:
                if node.module:
                    out.add(node.module)
                    # `from x import y` may name a submodule rather than an
                    # attribute, and which one it is cannot be known from the
                    # syntax. Both are recorded; a name that is not a module
                    # simply resolves to no file.
                    for alias in node.names:
                        out.add(f"{node.module}.{alias.name}")
            else:
                base = list(package[: len(package) - (node.level - 1)]) if node.level > 1 else list(
                    package
                )
                prefix = ".".join(base + ([node.module] if node.module else []))
                if prefix:
                    out.add(prefix)
                    for alias in node.names:
                        out.add(f"{prefix}.{alias.name}")
    return out


def import_closure(module: str, root: pathlib.Path = REPO) -> tuple[set[str], dict[str, str]]:
    """Every module reachable from `module`, and how each was reached.

    First-party modules are followed; anything else (numpy, pandas, the standard
    library, the bindings) is recorded and not descended into -- there is nothing
    to descend into for the bindings, which are a compiled extension.

    Returns the reachable names and, for each, the module that imported it, so a
    failure can print the path rather than only the verdict.
    """
    start = module_path(root, module)
    if start is None:
        raise AssertionError(f"{module} has no file under {root}; the guard is looking in the wrong place")

    seen: set[str] = {module}
    via: dict[str, str] = {}
    queue = [module]

    while queue:
        current = queue.pop()
        path = module_path(root, current)
        if path is None:
            continue
        for imported in sorted(direct_imports(path, root)):
            if imported not in seen:
                seen.add(imported)
                via[imported] = current
                queue.append(imported)

    return seen, via


def chain_to(target: str, via: dict[str, str], start: str) -> list[str]:
    """The import path from `start` to `target`, for the failure message."""
    chain = [target]
    while chain[-1] in via and chain[-1] != start:
        chain.append(via[chain[-1]])
    return list(reversed(chain))


def touches_bindings(module: str, root: pathlib.Path = REPO) -> list[str]:
    """The import chain reaching the bindings, or an empty list."""
    reachable, via = import_closure(module, root)
    for name in sorted(reachable):
        if name == BINDINGS or name.startswith(f"{BINDINGS}."):
            return chain_to(name, via, module)
    return []


@pytest.mark.parametrize("module", sorted(GUARDED))
def test_guarded_module_does_not_reach_the_cpp_bindings(module: str) -> None:
    chain = touches_bindings(module)
    assert not chain, (
        f"{module} imports the C++ bindings, directly or transitively:\n"
        f"    {' -> '.join(chain)}\n\n"
        f"{module} is {GUARDED[module]}.\n\n"
        "That comparison is evidence only while the two implementations are causally\n"
        "independent. Routed through the engine, the Python half stops being a second\n"
        "implementation and the test compares the engine with itself -- which passes\n"
        "perfectly, forever, whether or not the engine is correct, and prints exactly\n"
        "the same numbers it prints today.\n\n"
        "The rationale is in docs/AUDIT.md under 'Deliberate duplication'. If the\n"
        "duplication is genuinely to be abandoned, that is a decision to take there\n"
        "first, not a side effect of an import."
    )


def test_the_guard_examines_a_real_and_non_trivial_import_graph() -> None:
    """A closure of one module would pass the test above while checking nothing.

    The two guarded modules sit at opposite ends of the interesting range, which is
    why both are asserted rather than just the bigger one:

      research.sizing  imports nothing first-party at all -- stdlib only. Its
                       independence from the engine is structural rather than
                       maintained, which is the strongest form it could take.
      research.parity  reaches four other research modules, so the walk has real
                       first-party edges to follow here and is shown following
                       them against the actual tree rather than only in tmp_path.
    """
    sizing, _ = import_closure("research.sizing")
    assert len(sizing) > 3, f"research.sizing reached only {sizing}; the walk is not walking"
    assert not [n for n in sizing if n.startswith("research.") and n != "research.sizing"], (
        "research.sizing has grown a first-party import. That is not forbidden, but this "
        "test documented it as having none, so check what was added and update the comment."
    )

    parity, _ = import_closure("research.parity")
    first_party = {n for n in parity if n.startswith("research.") and n != "research.parity"}
    assert len(first_party) >= 4, (
        f"research.parity reached only {sorted(first_party)}; the walk is not following "
        "first-party edges, so the guard has a hole in it"
    )


def test_the_guard_fires_on_a_direct_import(tmp_path: pathlib.Path) -> None:
    """Introduce the forbidden import and require the guard to catch it."""
    package = tmp_path / "research"
    package.mkdir()
    (package / "__init__.py").write_text("", encoding="utf-8")
    (package / "sizing.py").write_text(
        textwrap.dedent(
            """
            import marketscan

            def grid():
                return marketscan.run_backtest()
            """
        ),
        encoding="utf-8",
    )

    chain = touches_bindings("research.sizing", root=tmp_path)
    assert chain == ["research.sizing", "marketscan"]


def test_the_guard_fires_on_a_transitive_import(tmp_path: pathlib.Path) -> None:
    """The realistic version: a helper picks up the bindings, not the module itself.

    This is the case worth having a test for. Nobody is going to write
    `import marketscan` at the top of parity.py; someone is going to add a
    convenience module that uses the engine, import it for something unrelated,
    and never notice what it dragged in.
    """
    package = tmp_path / "research"
    package.mkdir()
    (package / "__init__.py").write_text("", encoding="utf-8")
    (package / "parity.py").write_text(
        "from research.helpers import load_panel\n", encoding="utf-8"
    )
    (package / "helpers.py").write_text(
        "import marketscan\n\ndef load_panel():\n    return marketscan.load()\n",
        encoding="utf-8",
    )

    chain = touches_bindings("research.parity", root=tmp_path)
    assert chain == ["research.parity", "research.helpers", "marketscan"]


def test_the_guard_follows_relative_imports(tmp_path: pathlib.Path) -> None:
    """`from . import x` must be followed, or the guard has a hole in it."""
    package = tmp_path / "research"
    package.mkdir()
    (package / "__init__.py").write_text("", encoding="utf-8")
    (package / "sizing.py").write_text("from . import engine_bridge\n", encoding="utf-8")
    (package / "engine_bridge.py").write_text("import marketscan\n", encoding="utf-8")

    chain = touches_bindings("research.sizing", root=tmp_path)
    assert chain[0] == "research.sizing"
    assert chain[-1] == "marketscan"


def test_a_clean_module_is_not_flagged(tmp_path: pathlib.Path) -> None:
    """The guard must be capable of passing, or it is not testing anything."""
    package = tmp_path / "research"
    package.mkdir()
    (package / "__init__.py").write_text("", encoding="utf-8")
    (package / "sizing.py").write_text(
        "import math\n\nfrom research.panel import build\n", encoding="utf-8"
    )
    (package / "panel.py").write_text("import numpy\n", encoding="utf-8")

    assert touches_bindings("research.sizing", root=tmp_path) == []


def test_no_guarded_module_imports_dynamically() -> None:
    """The obvious way round an import-graph guard, closed.

    `importlib.import_module("marketscan")` or `__import__("marketscan")` is
    invisible to the walk above, because the module name is a string at runtime
    rather than a name in the syntax. Rather than leave that as a documented hole,
    dynamic import machinery is refused outright inside the guarded closure.

    This is stricter than it strictly needs to be -- a dynamic import of something
    harmless would also fail -- and that is the right trade here. The guarded
    modules are two self-contained numerical implementations; neither has any
    business loading a module by name, so the false-positive cost is close to zero
    and the alternative is a guard with a one-line bypass.
    """
    forbidden = ("importlib", "__import__")

    for module in GUARDED:
        reachable, via = import_closure(module)
        for name in sorted(reachable):
            path = module_path(REPO, name)
            if path is None or not name.startswith("research."):
                continue
            text = path.read_text(encoding="utf-8")
            for marker in forbidden:
                assert marker not in text, (
                    f"{name} uses {marker}, reached from {module} "
                    f"via {' -> '.join(chain_to(name, via, module))}.\n\n"
                    "A module name resolved at runtime is invisible to the import-graph "
                    "guard in this file, so it is the one-line way to route a guarded "
                    "implementation through the C++ bindings without the guard noticing.\n\n"
                    "See docs/AUDIT.md under 'Deliberate duplication'."
                )


def test_the_bindings_are_not_banned_outside_the_guarded_modules() -> None:
    """Scope, asserted.

    The point of naming modules individually is that everything else stays free to
    use the bindings. If this repository's legitimate users of them ever fell
    inside the guarded set, the guard would have overreached -- so that is checked
    rather than assumed.
    """
    legitimate = [
        "scripts/make_quickstart_notebook.py",
        "bindings/python/examples/smoke_test.py",
    ]

    present = [p for p in legitimate if (REPO / p).is_file()]
    assert present, "none of the known binding users exist; this test is checking nothing"

    for relative in present:
        text = (REPO / relative).read_text(encoding="utf-8")
        assert BINDINGS in text, f"{relative} no longer mentions the bindings; update this test"

    # And none of them is reachable from a guarded module, which is what makes
    # their use of the bindings none of the guard's business.
    for module in GUARDED:
        reachable, _ = import_closure(module)
        for relative in present:
            dotted = relative.replace("/", ".").removesuffix(".py")
            assert dotted not in reachable
