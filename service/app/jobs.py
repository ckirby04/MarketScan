"""Running ms_research as a job, and knowing what each job is doing.

The engine stays a command line: a form in, a run directory out, and with
--machine one JSON event per line on stdout. This module starts it, reads its
events into a status, and when it finishes writes that status beside the run
and hands the directory to the store.

A status is one of: queued, running, done, refused, failed. "refused" means the
form was not valid or the job could not honour it, with every reason by form
path; "failed" means the engine or the service broke.
"""

from __future__ import annotations

import json
import subprocess
import threading
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path

from .config import Settings


@dataclass
class Status:
    job_id: str
    state: str = "queued"
    stage: str = ""
    done: int = 0
    total: int = 0
    grade: str | None = None
    errors: list[dict] = field(default_factory=list)
    created_at: float = field(default_factory=time.time)
    finished_at: float | None = None

    def to_json(self) -> dict:
        return asdict(self)


class JobManager:
    def __init__(self, settings: Settings, store):
        self.settings = settings
        self.store = store
        self._jobs: dict[str, Status] = {}
        self._processes: dict[str, subprocess.Popen] = {}
        self._lock = threading.Lock()
        self._slots = threading.Semaphore(settings.max_running)

    def submit(self, form: dict) -> Status:
        job_id = uuid.uuid4().hex[:16]
        directory = self.settings.runs_dir / job_id
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "input.json").write_text(json.dumps(form), encoding="utf-8")
        status = Status(job_id)
        with self._lock:
            self._jobs[job_id] = status
        self._write_status(status)
        threading.Thread(target=self._run, args=(job_id,), daemon=True).start()
        return status

    def status(self, job_id: str) -> Status | None:
        with self._lock:
            live = self._jobs.get(job_id)
        if live is not None:
            return live
        raw = self.store.read(job_id, "status.json")
        if raw is None:
            return None
        return Status(**json.loads(raw))

    def cancel(self, job_id: str) -> bool:
        with self._lock:
            process = self._processes.get(job_id)
        if process is None:
            return False
        process.terminate()
        return True

    def recent(self, limit: int = 50) -> list[dict]:
        out = []
        for job_id in self.store.list_ids():
            status = self.status(job_id)
            if status is not None:
                out.append(status.to_json())
        out.sort(key=lambda s: s["created_at"], reverse=True)
        return out[:limit]

    # --- The worker ----------------------------------------------------------

    def _command(self, directory: Path) -> list[str]:
        s = self.settings
        command = [
            *s.ms_research,
            "--form", str(directory / "input.json"),
            "--out", str(directory),
            "--contracts", str(s.contracts),
            "--null-draws", str(s.null_draws),
            "--paths", str(s.bootstrap_paths),
            "--machine",
        ]
        if s.daily_dir is not None:
            command += ["--daily", str(s.daily_dir), "--data-snapshot", str(s.daily_dir)]
        for symbol, path in s.csv_sources:
            command += ["--csv", f"{symbol}={path}"]
        return command

    def _run(self, job_id: str) -> None:
        directory = self.settings.runs_dir / job_id
        with self._slots:
            status = self._jobs[job_id]
            status.state = "running"
            self._write_status(status)
            try:
                process = subprocess.Popen(
                    self._command(directory),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
            except OSError as error:
                self._finish(status, "failed", [{"path": "", "message": str(error)}])
                return
            with self._lock:
                self._processes[job_id] = process

            ended = None
            # The context closes both pipes and reaps the process, whatever
            # happens while reading them.
            with process:
                for line in process.stdout:
                    try:
                        event = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    kind = event.get("event")
                    if kind == "progress":
                        status.stage = event.get("stage", "")
                        status.done = event.get("done", 0)
                        status.total = event.get("total", 0)
                    elif kind in ("done", "refused"):
                        ended = event
                stderr = process.stderr.read()
            code = process.wait()
            with self._lock:
                self._processes.pop(job_id, None)

            if ended and ended["event"] == "done" and code == 0:
                status.grade = ended.get("grade")
                self._finish(status, "done", [])
            elif ended and ended["event"] == "refused":
                state = "failed" if ended.get("kind") == "engine_bug" else "refused"
                self._finish(status, state, ended.get("errors", []))
            elif code < 0 or code == 143:
                self._finish(status, "failed", [{"path": "", "message": "cancelled"}])
            else:
                message = stderr.strip().splitlines()[-1] if stderr.strip() else f"exit {code}"
                self._finish(status, "failed", [{"path": "", "message": message}])

    def _finish(self, status: Status, state: str, errors: list[dict]) -> None:
        status.state = state
        status.errors = errors
        status.finished_at = time.time()
        self._write_status(status)
        self.store.save(status.job_id, self.settings.runs_dir / status.job_id)

    def _write_status(self, status: Status) -> None:
        path = self.settings.runs_dir / status.job_id / "status.json"
        path.write_text(json.dumps(status.to_json()), encoding="utf-8")
