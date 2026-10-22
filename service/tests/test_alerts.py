"""Email alerts on finished runs (docs/specs/SPEC_SCORECARD_ALERTS.md section 5).

The rule and the email are tested directly; then the whole path through the
service, with the fake engine, whose Results are the committed fixture: a
fragile run whose scorecard is an A, the case the robustness floor is for.
Nothing here reaches the network: the sender is replaced.
"""

from __future__ import annotations

import copy
import json
import sys
import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

SERVICE = Path(__file__).resolve().parents[1]
REPO = SERVICE.parent
sys.path.insert(0, str(SERVICE))

from app import alerts, config  # noqa: E402
from app.main import create_app  # noqa: E402

FIXTURE = json.loads((REPO / "data" / "samples" / "research" / "job_results.json").read_text())
FORM = json.loads((REPO / "data" / "samples" / "research" / "job_form.json").read_text())
KEY = "test-key"
AUTH = {"Authorization": f"Bearer {KEY}"}


def results(score: float, grade: str) -> dict:
    out = copy.deepcopy(FIXTURE)
    out["scorecard"]["overall"]["score"] = score
    out["verdict"]["grade"] = grade
    return out


def test_the_fixture_is_the_case_the_floor_is_for():
    # If the fixture stops being a high-scoring fragile run, the tests below
    # stop testing the robustness floor.
    assert FIXTURE["verdict"]["grade"] == "fragile"
    assert FIXTURE["scorecard"]["overall"]["score"] >= 80


@pytest.mark.parametrize(
    "score, grade, floor, clears",
    [
        (85.0, "supported", "supported", True),
        (80.0, "supported", "supported", True),  # at the threshold
        (79.9, "supported", "supported", False),
        (95.0, "fragile", "supported", False),  # the floor holds back a lucky pick
        (95.0, "fragile", "fragile", True),
        (95.0, "insufficient_evidence", "", True),  # no floor
    ],
)
def test_the_rule(score, grade, floor, clears):
    assert alerts.decide(results(score, grade), 80.0, floor)[0] is clears


def test_results_without_a_scorecard_never_alert():
    old = copy.deepcopy(FIXTURE)
    del old["scorecard"]
    assert alerts.decide(old, 0.0, "") == (False, "no scorecard in these Results")


def test_the_email_says_history_not_advice():
    form = dict(FORM, prompt="Does buying after a 20% drop beat holding?")
    email = alerts.compose(FIXTURE, form, "me@example.com", "https://app.example/runs/abc")
    assert email.to == "me@example.com"
    assert "89 (A)" in email.subject and "historical" in email.subject
    assert "fragile (unverified data)" in email.text
    assert "https://app.example/runs/abc" in email.text
    assert "Versus holding: A" in email.text
    # The app's own words never advise. The user's question is quoted as theirs.
    own = email.subject + email.text.replace(form["prompt"], "")
    for banned in ("buy", "recommend", "expected return", "good idea"):
        assert banned not in own.lower(), banned


# --- Through the service ---------------------------------------------------------


def wait(client, job_id, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        status = client.get(f"/jobs/{job_id}", headers=AUTH).json()
        if status["state"] in ("done", "refused", "failed"):
            return status
        time.sleep(0.05)
    raise AssertionError(f"job {job_id} did not finish")


@pytest.fixture()
def sent(monkeypatch):
    outbox = []
    monkeypatch.setattr(alerts, "send_with_resend", lambda key, sender, email: outbox.append(email))
    return outbox


def service(tmp_path, **alert_settings):
    settings = config.Settings(
        api_key=KEY,
        ms_research=(sys.executable, str(SERVICE / "tests" / "fake_ms_research.py")),
        runs_dir=tmp_path / "runs",
        **alert_settings,
    )
    return TestClient(create_app(settings)), settings.runs_dir


def run(client, runs_dir) -> dict:
    job = client.post("/jobs", json=FORM, headers=AUTH).json()
    assert wait(client, job["job_id"])["state"] == "done"
    return json.loads((runs_dir / job["job_id"] / "alert.json").read_text())


def test_a_run_that_clears_the_rule_sends_one_email(tmp_path, sent):
    client, runs = service(tmp_path, alert_email_to="me@example.com", resend_api_key="k",
                           alert_min_robustness="", public_url="https://app.example/")
    record = run(client, runs)
    assert record["sent"] is True
    assert len(sent) == 1
    assert "/runs/" + record["job_id"] in sent[0].text


def test_the_robustness_floor_holds_back_the_fragile_fixture(tmp_path, sent):
    client, runs = service(tmp_path, alert_email_to="me@example.com", resend_api_key="k")
    record = run(client, runs)
    assert record["sent"] is False
    assert "fragile, below supported" in record["reason"]
    assert sent == []


def test_without_an_address_or_a_key_nothing_is_sent(tmp_path, sent):
    client, runs = service(tmp_path, resend_api_key="k", alert_min_robustness="")
    assert run(client, runs)["reason"] == "no alert address is set"
    client, runs = service(tmp_path / "b", alert_email_to="me@example.com",
                           alert_min_robustness="")
    assert run(client, runs)["reason"] == "no email key is set"
    assert sent == []


def test_a_failed_email_does_not_fail_the_job(tmp_path, monkeypatch):
    def broken(key, sender, email):
        raise RuntimeError("the email service answered 500")

    monkeypatch.setattr(alerts, "send_with_resend", broken)
    client, runs = service(tmp_path, alert_email_to="me@example.com", resend_api_key="k",
                           alert_min_robustness="")
    record = run(client, runs)  # asserts the job is done
    assert record["sent"] is False
    assert "500" in record["error"]
