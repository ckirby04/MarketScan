#!/usr/bin/env python3
"""Executes the quickstart notebook in place, so the committed copy has output.

A notebook with no output is a notebook nobody can read without running it, and
one whose output was written by hand is worse than that. This runs it for real.

The tooling goes into a virtual environment under out/, not into the system
Python: nbclient, ipykernel and matplotlib are needed to produce the artifact
and have nothing to do with using MarketScan.

    python scripts/run_notebook.py

The venv's interpreter must be able to import the built module, which means it
must be the same minor version the module was built for. The script checks.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import venv
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VENV_DIR = REPO / "out" / "notebook-venv"
NOTEBOOK = REPO / "bindings" / "python" / "examples" / "quickstart.ipynb"
REQUIREMENTS = ["nbclient", "nbformat", "ipykernel", "matplotlib"]


def module_dir() -> Path:
    for candidate in sorted(REPO.glob("out/build/*/bin")):
        if any(candidate.glob("marketscan*.pyd")):
            return candidate
    raise SystemExit(
        "No built Python module found. Build the full preset first:\n"
        "  ./scripts/dev-env.ps1 cmake --preset full\n"
        "  ./scripts/dev-env.ps1 cmake --build --preset full"
    )


def module_python_tag(directory: Path) -> str:
    """'cp312' from marketscan.cp312-win_amd64.pyd."""
    for candidate in directory.glob("marketscan*.pyd"):
        parts = candidate.name.split(".")
        if len(parts) >= 2 and parts[1].startswith("cp"):
            return parts[1].split("-")[0]
    return ""


def venv_python(directory: Path) -> Path:
    windows = directory / "Scripts" / "python.exe"
    return windows if windows.exists() else directory / "bin" / "python"


def ensure_venv(base_python: Path) -> Path:
    python = venv_python(VENV_DIR)
    if not python.exists():
        print(f"creating {VENV_DIR.relative_to(REPO)} from {base_python}")
        subprocess.run(
            [str(base_python), "-m", "venv", str(VENV_DIR)],
            check=True,
        )
        python = venv_python(VENV_DIR)

    print("installing notebook tooling")
    subprocess.run(
        [str(python), "-m", "pip", "install", "--quiet", "--upgrade", "pip"],
        check=True,
    )
    subprocess.run(
        [str(python), "-m", "pip", "install", "--quiet", *REQUIREMENTS],
        check=True,
    )
    return python


def find_base_python(required_tag: str) -> Path:
    """An interpreter whose version matches the module's ABI tag."""
    wanted = required_tag.removeprefix("cp")  # "312"
    wanted_version = f"{wanted[0]}.{wanted[1:]}"

    candidates: list[Path] = []
    if sys.platform == "win32":
        try:
            listing = subprocess.run(
                ["py", "-0p"], capture_output=True, text=True, check=False
            ).stdout
            for line in listing.splitlines():
                if f"-V:{wanted_version}" in line:
                    path = line.split(None, 1)[-1].strip().strip("*").strip()
                    if path:
                        candidates.append(Path(path))
        except FileNotFoundError:
            pass

    current = Path(sys.executable)
    if f"{sys.version_info.major}.{sys.version_info.minor}" == wanted_version:
        candidates.append(current)

    for candidate in candidates:
        if candidate.exists():
            return candidate

    raise SystemExit(
        f"The module was built for Python {wanted_version} "
        f"({required_tag}), and no such interpreter was found.\n"
        f"Install Python {wanted_version}, or rebuild the module against the "
        f"one you have with -DMARKETSCAN_PYTHON_EXECUTABLE."
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=int, default=600,
                        help="Seconds allowed per cell.")
    args = parser.parse_args()

    built = module_dir()
    tag = module_python_tag(built)
    print(f"module: {built} ({tag or 'unknown ABI tag'})")

    base = find_base_python(tag) if tag else Path(sys.executable)
    python = ensure_venv(base)

    runner = f"""
import nbformat
from nbclient import NotebookClient

path = r'{NOTEBOOK}'
notebook = nbformat.read(path, as_version=4)
client = NotebookClient(
    notebook,
    timeout={args.timeout},
    kernel_name='python3',
    resources={{'metadata': {{'path': r'{REPO}'}}}},
)
client.execute()
nbformat.write(notebook, path)
print('executed', len(notebook.cells), 'cells')
"""

    print(f"executing {NOTEBOOK.relative_to(REPO)}")
    completed = subprocess.run([str(python), "-c", runner], check=False)
    if completed.returncode != 0:
        print("notebook execution failed", file=sys.stderr)
        return completed.returncode

    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
