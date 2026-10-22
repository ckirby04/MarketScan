"""The job service: the research engine behind HTTP.

    POST /jobs                 a Strategy Form in; a job id out (202)
    GET  /jobs                 recent jobs, newest first
    GET  /jobs/{id}            its status: queued, running (with progress),
                               done, refused (with every reason by form path),
                               or failed
    GET  /jobs/{id}/results    the Results document (results.schema.json)
    GET  /jobs/{id}/curves     the chart curves
    GET  /jobs/{id}/form       the form as run
    POST /jobs/{id}/cancel     stop a running job; no partial Results are kept
    GET  /contracts/form-schema  strategy_form.schema.json, for a form editor
    GET  /health               no auth; for the platform's checks

Run with `uvicorn app.main:create_app --factory`: the app is built when the
server starts, from the environment then, rather than as a side effect of
importing this module.

Every route but /health needs `Authorization: Bearer <MS_API_KEY>`. The web front
end calls these from its server, so the key never reaches a browser.

A form is validated against the contract here, before an engine process is
started, with the research layer's own validator (research/contracts.py) --
the same one the cross-test corpus holds to the engine's.
"""

from __future__ import annotations

import hmac
import json
import sys

from fastapi import Depends, FastAPI, Header, HTTPException, Response
from fastapi.responses import JSONResponse

from . import config
from .jobs import JobManager
from .storage import GcsStore, LocalStore, read_json

sys.path.insert(0, str(config.REPO))
from research import contracts  # noqa: E402  (the repository root is the path)


def create_app(settings: config.Settings | None = None) -> FastAPI:
    settings = settings or config.from_env()
    settings.runs_dir.mkdir(parents=True, exist_ok=True)
    local = LocalStore(settings.runs_dir)
    store = GcsStore(settings.runs_bucket, local) if settings.runs_bucket else local
    jobs = JobManager(settings, store)
    form_schema = json.loads(
        (settings.contracts / "strategy_form.schema.json").read_text(encoding="utf-8")
    )
    validator = contracts.validator(form_schema)

    app = FastAPI(title="MarketScan research service", version="0.1")

    def authorised(authorization: str = Header(default="")) -> None:
        expected = f"Bearer {settings.api_key}"
        if not settings.api_key or not hmac.compare_digest(authorization, expected):
            raise HTTPException(status_code=401, detail="unauthorised")

    def found(job_id: str):
        status = jobs.status(job_id)
        if status is None:
            raise HTTPException(status_code=404, detail="no such job")
        return status

    @app.get("/health")
    def health() -> dict:
        return {"ok": True}

    @app.get("/contracts/form-schema", dependencies=[Depends(authorised)])
    def form_schema_route() -> dict:
        return form_schema

    @app.post("/jobs", status_code=202, dependencies=[Depends(authorised)])
    def submit(form: dict) -> JSONResponse:
        errors = [
            {"path": "/" + "/".join(str(p) for p in e.absolute_path), "message": e.message}
            for e in validator.iter_errors(form)
        ]
        if errors:
            return JSONResponse(status_code=422, content={"state": "refused", "errors": errors})
        return JSONResponse(status_code=202, content=jobs.submit(form).to_json())

    @app.get("/jobs", dependencies=[Depends(authorised)])
    def recent() -> list[dict]:
        return jobs.recent()

    @app.get("/jobs/{job_id}", dependencies=[Depends(authorised)])
    def status(job_id: str) -> dict:
        return found(job_id).to_json()

    def run_file(job_id: str, name: str):
        found(job_id)
        data = read_json(store, job_id, name)
        if data is None:
            raise HTTPException(status_code=404, detail=f"{name} is not ready")
        return data

    @app.get("/jobs/{job_id}/results", dependencies=[Depends(authorised)])
    def results(job_id: str):
        return run_file(job_id, "results.json")

    @app.get("/jobs/{job_id}/curves", dependencies=[Depends(authorised)])
    def curves(job_id: str):
        return run_file(job_id, "curves.json")

    @app.get("/jobs/{job_id}/form", dependencies=[Depends(authorised)])
    def form(job_id: str):
        return run_file(job_id, "form.json")

    @app.post("/jobs/{job_id}/cancel", dependencies=[Depends(authorised)])
    def cancel(job_id: str) -> Response:
        found(job_id)
        return Response(status_code=202 if jobs.cancel(job_id) else 409)

    return app

