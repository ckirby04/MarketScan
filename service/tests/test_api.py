"""The job service's HTTP surface, against a fake engine.

The engine itself is tested in tests/ and research/tests/. What is tested here is
everything the service adds: authentication, validation before any process is
started, the job lifecycle read from the --machine protocol, refusals carried
through by form path, cancellation, and run files served back.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

SERVICE = Path(__file__).resolve().parents[1]
REPO = SERVICE.parent
sys.path.insert(0, str(SERVICE))

from app import config  # noqa: E402
from app.main import create_app  # noqa: E402

KEY = "test-key"
AUTH = {"Authorization": f"Bearer {KEY}"}
FORM = json.loads((REPO / "data" / "samples" / "research" / "job_form.json").read_text())


@pytest.fixture()
def client(tmp_path):
    settings = config.Settings(
        api_key=KEY,
        ms_research=(sys.executable, str(SERVICE / "tests" / "fake_ms_research.py")),
        runs_dir=tmp_path / "runs",
    )
    return TestClient(create_app(settings))


def wait(client, job_id, until=("done", "refused", "failed"), timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        status = client.get(f"/jobs/{job_id}", headers=AUTH).json()
        if status["state"] in until:
            return status
        time.sleep(0.05)
    raise AssertionError(f"job {job_id} did not reach {until}")


def with_prompt(prompt: str) -> dict:
    form = dict(FORM)
    form["prompt"] = prompt
    return form


def test_health_needs_no_key(client):
    assert client.get("/health").json() == {"ok": True}


def test_every_other_route_needs_the_key(client):
    assert client.get("/jobs").status_code == 401
    assert client.get("/jobs", headers={"Authorization": "Bearer wrong"}).status_code == 401
    assert client.post("/jobs", json=FORM).status_code == 401


def test_an_empty_key_refuses_everyone(tmp_path):
    app = create_app(config.Settings(api_key="", runs_dir=tmp_path))
    assert TestClient(app).get("/jobs", headers={"Authorization": "Bearer "}).status_code == 401


def test_an_invalid_form_is_refused_before_any_engine_runs(client):
    form = dict(FORM)
    form["execution"] = {"fill": "same_close"}
    response = client.post("/jobs", json=form, headers=AUTH)
    assert response.status_code == 422
    paths = [e["path"] for e in response.json()["errors"]]
    assert "/execution/fill" in paths
    assert client.get("/jobs", headers=AUTH).json() == []


def test_a_job_runs_to_done_and_its_files_are_served(client):
    job = client.post("/jobs", json=FORM, headers=AUTH)
    assert job.status_code == 202
    status = wait(client, job.json()["job_id"])
    assert status["state"] == "done"
    assert status["grade"] == "fragile"
    job_id = status["job_id"]
    results = client.get(f"/jobs/{job_id}/results", headers=AUTH).json()
    assert results["verdict"]["grade"] == "fragile"
    assert client.get(f"/jobs/{job_id}/curves", headers=AUTH).json() == []
    assert client.get(f"/jobs/{job_id}/form", headers=AUTH).json()["template"] == "drawdown_entry"
    assert [j["job_id"] for j in client.get("/jobs", headers=AUTH).json()] == [job_id]


def test_a_refusal_carries_its_reasons_by_form_path(client):
    job_id = client.post("/jobs", json=with_prompt("refuse"), headers=AUTH).json()["job_id"]
    status = wait(client, job_id)
    assert status["state"] == "refused"
    assert status["errors"] == [{"path": "/execution/fill", "message": "no opening quote"}]
    assert client.get(f"/jobs/{job_id}/results", headers=AUTH).status_code == 404


def test_an_engine_that_dies_is_a_failure_with_its_last_words(client):
    job_id = client.post("/jobs", json=with_prompt("crash"), headers=AUTH).json()["job_id"]
    status = wait(client, job_id)
    assert status["state"] == "failed"
    assert status["errors"][0]["message"] == "something broke"


def test_a_running_job_can_be_cancelled_and_keeps_no_results(client):
    job_id = client.post("/jobs", json=with_prompt("slow"), headers=AUTH).json()["job_id"]
    status = wait(client, job_id, until=("running",))
    assert status["state"] == "running"
    time.sleep(0.2)
    assert client.post(f"/jobs/{job_id}/cancel", headers=AUTH).status_code == 202
    status = wait(client, job_id)
    assert status["state"] == "failed"
    assert client.get(f"/jobs/{job_id}/results", headers=AUTH).status_code == 404


def test_an_unknown_job_is_404(client):
    assert client.get("/jobs/nope", headers=AUTH).status_code == 404


def test_the_form_schema_is_served_for_a_form_editor(client):
    schema = client.get("/contracts/form-schema", headers=AUTH).json()
    assert schema["title"] == "Strategy Form"
