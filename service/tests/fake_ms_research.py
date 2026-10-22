"""Stands in for ms_research in the service's tests.

Speaks the same --machine protocol and writes the same run directory, so the
service's job handling can be tested without the engine. How it behaves is
chosen by the form's prompt:

    "refuse"   reports a refusal naming /execution/fill, exit 3
    "crash"    writes to stderr and exits 1 without an event
    "slow"     reports progress slowly, so a test can cancel it
    anything   reports progress and finishes with a copy of the committed
               fixture Results
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
from pathlib import Path

FIXTURES = Path(__file__).resolve().parents[2] / "data" / "samples" / "research"


def emit(event: dict) -> None:
    print(json.dumps(event), flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--form", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--machine", action="store_true")
    args, _ = parser.parse_known_args()

    form = json.loads(Path(args.form).read_text(encoding="utf-8"))
    out = Path(args.out)
    prompt = form.get("prompt", "")

    if prompt == "refuse":
        emit({"event": "refused", "kind": "refused",
              "errors": [{"path": "/execution/fill", "message": "no opening quote"}]})
        return 3
    if prompt == "crash":
        print("something broke", file=sys.stderr)
        return 1

    for i in range(3 if prompt != "slow" else 600):
        emit({"event": "progress", "stage": "sweep", "done": i, "total": 3})
        if prompt == "slow":
            time.sleep(0.05)

    out.mkdir(parents=True, exist_ok=True)
    (out / "form.json").write_text(json.dumps(form), encoding="utf-8")
    shutil.copy(FIXTURES / "job_results.json", out / "results.json")
    (out / "curves.json").write_text("[]", encoding="utf-8")
    (out / "equity.csv").write_text("date,strategy\n", encoding="utf-8")
    results = json.loads((out / "results.json").read_text(encoding="utf-8"))
    emit({"event": "done", "grade": results["verdict"]["grade"],
          "run_id": results["run"]["run_id"]})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
