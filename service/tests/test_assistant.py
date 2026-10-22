"""The assistant's conversation loop, against a scripted model and the fake engine.

No model is called. A fake client plays back one scripted reply per model call
and records every request, so what is tested is everything the service does
around the model: the tools it serves and what they answer, questions held
until they are answered, the form checked before it can run, the run started
and its Results sent back for the analysis, and a history that is only ever
appended to -- the condition for the model's thinking to survive from one call
to the next.
"""

from __future__ import annotations

import copy
import json
import sys
import time
from pathlib import Path
from types import SimpleNamespace

import pytest
from fastapi.testclient import TestClient

SERVICE = Path(__file__).resolve().parents[1]
REPO = SERVICE.parent
sys.path.insert(0, str(SERVICE))
sys.path.insert(0, str(REPO / "scripts"))

from app import config  # noqa: E402
from app.catalog import Catalog  # noqa: E402
from app.main import create_app  # noqa: E402

KEY = "test-key"
AUTH = {"Authorization": f"Bearer {KEY}"}
FORM = json.loads((REPO / "data" / "samples" / "research" / "job_form.json").read_text())
FIXTURE_CSV = REPO / "data" / "samples" / "research" / "drawdown_prices.csv"


# --- A scripted model ----------------------------------------------------------

def text(value):
    return {"type": "text", "text": value}


def tool(name, tool_input, tool_id=None):
    return {"type": "tool_use", "id": tool_id or f"toolu_{name}", "name": name, "input": tool_input}


class Block(SimpleNamespace):
    def to_dict(self):
        return {k: v for k, v in vars(self).items()}


class Stream:
    def __init__(self, reply):
        self.reply = reply

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def __iter__(self):
        for block in self.reply["content"]:
            if block["type"] == "text":
                # Two pieces, as a real stream delivers text in deltas.
                half = len(block["text"]) // 2
                yield SimpleNamespace(type="text", text=block["text"][:half])
                yield SimpleNamespace(type="text", text=block["text"][half:])

    def get_final_message(self):
        return SimpleNamespace(
            content=[Block(**b) for b in self.reply["content"]],
            stop_reason=self.reply.get("stop_reason") or (
                "tool_use" if any(b["type"] == "tool_use" for b in self.reply["content"]) else "end_turn"),
            model="scripted",
            usage=SimpleNamespace(input_tokens=1, output_tokens=1,
                                  cache_read_input_tokens=0, cache_creation_input_tokens=0),
        )


class ScriptedModel:
    def __init__(self):
        self.replies: list[dict] = []
        self.requests: list[dict] = []
        self.beta = SimpleNamespace(messages=SimpleNamespace(stream=self.stream))

    def script(self, *replies):
        self.replies.extend(replies)

    def stream(self, **request):
        self.requests.append(copy.deepcopy(request))
        assert self.replies, "the model was called more times than scripted"
        return Stream(self.replies.pop(0))


@pytest.fixture()
def model():
    return ScriptedModel()


@pytest.fixture()
def client(tmp_path, model):
    settings = config.Settings(
        api_key=KEY,
        ms_research=(sys.executable, str(SERVICE / "tests" / "fake_ms_research.py")),
        runs_dir=tmp_path / "runs",
        csv_sources=(("FIXTURE", FIXTURE_CSV),),
    )
    return TestClient(create_app(settings, assistant_client=model))


def events(response) -> list[dict]:
    assert response.status_code == 200, response.text
    return [json.loads(line) for line in response.text.splitlines() if line.strip()]


def new_conversation(client) -> str:
    response = client.post("/conversations", headers=AUTH)
    assert response.status_code == 201
    return response.json()["id"]


def say(client, conversation_id, words):
    return events(client.post(f"/conversations/{conversation_id}/messages",
                              json={"text": words}, headers=AUTH))


def wait(client, job_id, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        status = client.get(f"/jobs/{job_id}", headers=AUTH).json()
        if status["state"] in ("done", "refused", "failed"):
            return status
        time.sleep(0.05)
    raise AssertionError(f"job {job_id} did not end")


def with_missing_exit() -> dict:
    draft = copy.deepcopy(FORM)
    draft["provenance"] = {"/strategy/exit/rules": {"source": "missing", "options": ["hold", "recover"]}}
    return draft


QUESTION = {"path": "/strategy/exit/rules", "question": "When should it sell?",
            "options": [{"label": "Hold", "value": []},
                        {"label": "At the old high", "value": [{"type": "recover_to_high"}]}]}


# --- The whole path --------------------------------------------------------------

def test_a_question_becomes_a_run_and_its_results_an_analysis(client, model):
    conversation_id = new_conversation(client)
    model.script(
        {"content": [tool("search_securities", {"query": ""}, "t1"),
                     tool("resolve", {"ticker": "FIXTURE", "as_of": "2000-01-03"}, "t2")]},
        {"content": [tool("save_form", {"form": with_missing_exit(), "ready": False}, "t3"),
                     tool("ask_user", {"message": "The data starts in 2000.", "questions": [QUESTION]}, "t4")]},
    )
    first = say(client, conversation_id, "what drawdown should I buy FIXTURE at?")
    kinds = [e["event"] for e in first]
    assert kinds[0] == "item" and first[0]["item"] == {"kind": "user", "text": "what drawdown should I buy FIXTURE at?"}
    assert "activity" in kinds and kinds[-1] == "end"
    assert first[-2]["item"]["kind"] == "questions"
    assert first[-2]["item"]["message"] == "The data starts in 2000."

    # The tools answered from the data held.
    search = json.loads(model.requests[1]["messages"][2]["content"][0]["content"])
    assert [s["security_id"] for s in search["securities"]] == ["fixture"]
    resolved = json.loads(model.requests[1]["messages"][2]["content"][1]["content"])
    assert resolved["security"]["ticker_history"][0]["ticker"] == "FIXTURE"

    # The answer goes back with the save_form result, in one user message.
    model.script(
        {"content": [tool("save_form", {"form": FORM, "ready": True}, "t5")]},
        {"content": [text("This will test buying FIXTURE after each drawdown from 10% to 40%.")]},
    )
    second = say(client, conversation_id, "At the old high")
    answer = model.requests[2]["messages"][-1]
    assert answer["role"] == "user"
    assert [b["tool_use_id"] for b in answer["content"]] == ["t3", "t4"]
    assert answer["content"][1]["content"] == "At the old high"
    assert any(e["event"] == "item" and e["item"]["kind"] == "ready" for e in second)
    streamed = "".join(e["delta"] for e in second if e["event"] == "text")
    assert streamed == "This will test buying FIXTURE after each drawdown from 10% to 40%."

    shown = client.get(f"/conversations/{conversation_id}", headers=AUTH).json()
    assert shown["ready"] is True
    assert [i["kind"] for i in shown["display"]] == ["user", "questions", "user", "ready", "assistant"]
    assert shown["display"][1]["answered"] is True
    assert "messages" not in shown and "system" not in shown

    # Run, then the Results go to the model and its reply is the analysis.
    started = client.post(f"/conversations/{conversation_id}/run", headers=AUTH)
    assert started.status_code == 202
    job_id = started.json()["job_id"]
    assert wait(client, job_id)["state"] == "done"
    model.script({"content": [text("History doesn't settle this: two trades carried it.")]})
    analysis = events(client.post(f"/conversations/{conversation_id}/results",
                                  json={"job_id": job_id}, headers=AUTH))
    assert analysis[-1] == {"event": "end"}
    sent = model.requests[-1]["messages"][-1]["content"][-1]["text"]
    assert sent.startswith(f'<results job_id="{job_id}">')
    assert json.loads(sent.split("\n", 1)[1].rsplit("\n", 1)[0])["verdict"]["grade"]

    shown = client.get(f"/conversations/{conversation_id}", headers=AUTH).json()
    assert shown["display"][-2] == {"kind": "run", "job_id": job_id}
    assert shown["display"][-1] == {"kind": "analysis", "job_id": job_id,
                                    "text": "History doesn't settle this: two trades carried it."}
    assert shown["analysed"] == [job_id]
    # A run is analysed once.
    again = client.post(f"/conversations/{conversation_id}/results", json={"job_id": job_id}, headers=AUTH)
    assert again.status_code == 409

    # Every call saw the same frozen prompt and tools, and a history that only
    # grew: each request's messages begin with the previous request's.
    assert len({json.dumps(r["system"]) for r in model.requests}) == 1
    assert len({json.dumps(r["tools"]) for r in model.requests}) == 1
    for before, after in zip(model.requests, model.requests[1:]):
        assert after["messages"][:len(before["messages"])] == before["messages"]
    assert [t["name"] for t in model.requests[0]["tools"]] == [
        "search_securities", "resolve", "get_coverage", "save_form", "ask_user", "start_run"]
    assert "{{" not in model.requests[0]["system"]


def test_the_model_can_start_the_run_when_asked(client, model):
    conversation_id = new_conversation(client)
    model.script(
        {"content": [tool("save_form", {"form": FORM, "ready": True}, "a")]},
        {"content": [tool("start_run", {}, "b")]},
        {"content": [text("Running now.")]},
    )
    out = say(client, conversation_id, "test FIXTURE drawdowns and just run it")
    runs = [e["item"] for e in out if e["event"] == "item" and e["item"]["kind"] == "run"]
    assert len(runs) == 1
    result = json.loads(model.requests[2]["messages"][-1]["content"][0]["content"])
    assert result["job_id"] == runs[0]["job_id"]
    assert wait(client, runs[0]["job_id"])["state"] == "done"


def test_a_form_is_not_accepted_with_schema_errors_or_missing_values(client, model):
    conversation_id = new_conversation(client)
    broken = copy.deepcopy(FORM)
    broken["execution"] = {"fill": "same_close"}
    model.script(
        {"content": [tool("save_form", {"form": broken, "ready": True}, "a")]},
        {"content": [tool("save_form", {"form": with_missing_exit(), "ready": True}, "b")]},
        {"content": [tool("start_run", {}, "c")]},
        {"content": [text("I need to know when to sell first.")]},
    )
    say(client, conversation_id, "test something")
    first = json.loads(model.requests[1]["messages"][-1]["content"][0]["content"])
    assert first["accepted"] is False
    assert "/execution/fill" in [e["path"] for e in first["errors"]]
    second = json.loads(model.requests[2]["messages"][-1]["content"][0]["content"])
    assert second["accepted"] is False
    assert second["errors"] == [{"path": "/strategy/exit/rules",
                                 "message": "still marked missing: ask, or resolve it"}]
    refused = model.requests[3]["messages"][-1]["content"][0]
    assert refused["is_error"] is True and "no accepted form" in refused["content"]
    shown = client.get(f"/conversations/{conversation_id}", headers=AUTH).json()
    assert shown["ready"] is False and shown["runs"] == []
    assert client.post(f"/conversations/{conversation_id}/run", headers=AUTH).status_code == 409


def test_a_refused_run_goes_back_to_the_model_to_fix(client, model):
    conversation_id = new_conversation(client)
    refusing = copy.deepcopy(FORM)
    refusing["prompt"] = "refuse"  # the fake engine refuses this one
    model.script({"content": [tool("save_form", {"form": refusing, "ready": True}, "a")]},
                 {"content": [text("Ready.")]})
    say(client, conversation_id, "refuse")
    job_id = client.post(f"/conversations/{conversation_id}/run", headers=AUTH).json()["job_id"]
    assert wait(client, job_id)["state"] == "refused"
    model.script({"content": [text("The job can't fill on the same bar; I changed that.")]})
    out = events(client.post(f"/conversations/{conversation_id}/results", json={"job_id": job_id}, headers=AUTH))
    assert out[-1]["event"] == "end"
    sent = model.requests[-1]["messages"][-1]["content"][-1]["text"]
    assert sent.startswith(f'<refused job_id="{job_id}" state="refused">')
    assert "/execution/fill" in sent
    shown = client.get(f"/conversations/{conversation_id}", headers=AUTH).json()
    assert shown["display"][-1]["kind"] == "assistant"


def test_invalid_tool_input_and_unknown_tools_are_the_models_to_hear_about(client, model):
    conversation_id = new_conversation(client)
    model.script(
        {"content": [tool("resolve", {"ticker": "FIXTURE"}, "a"), tool("list_regimes", {}, "b")]},
        {"content": [text("Sorry.")]},
    )
    say(client, conversation_id, "hello")
    results = model.requests[1]["messages"][-1]["content"]
    assert all(r["is_error"] for r in results)
    assert "as_of" in results[0]["content"]
    assert "no tool named list_regimes" in results[1]["content"]


def test_a_failed_model_call_ends_the_turn_and_keeps_the_conversation(client, model):
    conversation_id = new_conversation(client)
    out = say(client, conversation_id, "hello")  # nothing scripted: the call fails
    assert [e["event"] for e in out][-2:] == ["error", "end"]
    shown = client.get(f"/conversations/{conversation_id}", headers=AUTH).json()
    assert [i["kind"] for i in shown["display"]] == ["user", "error"]
    model.script({"content": [text("Hello.")]})
    say(client, conversation_id, "hello again")
    # Two user messages in a row: the API joins them into one turn.
    assert [m["role"] for m in model.requests[-1]["messages"]] == ["user", "user"]


def test_conversations_are_listed_newest_first_and_need_the_key(client, model):
    assert client.get("/conversations").status_code == 401
    first = new_conversation(client)
    model.script({"content": [text("One.")]}, {"content": [text("Two.")]})
    say(client, first, "first question")
    second = new_conversation(client)
    say(client, second, "second question")
    new_conversation(client)  # nothing said yet: not listed
    listed = client.get("/conversations", headers=AUTH).json()
    assert [c["title"] for c in listed] == ["second question", "first question"]
    assert client.get("/conversations/nope", headers=AUTH).status_code == 404


def test_without_a_key_the_assistant_answers_503_and_jobs_still_work(tmp_path):
    app = create_app(config.Settings(api_key=KEY, runs_dir=tmp_path))
    plain = TestClient(app)
    assert plain.post("/conversations", headers=AUTH).status_code == 503
    assert plain.get("/conversations", headers=AUTH).json() == []
    assert plain.get("/jobs", headers=AUTH).status_code == 200


# --- The data the tools answer from ----------------------------------------------

def test_the_catalog_reads_dates_from_msb_files_and_the_manifest(tmp_path):
    from fetch_stooq import closing_bell, write_msb  # the writer the engine's files come from
    import datetime as dt

    bars = [(closing_bell(dt.date(2010, 2, 11)), 1, 1, 1, 1, 0),
            (closing_bell(dt.date(2010, 2, 12)), 1, 1, 1, 1, 0),
            (closing_bell(dt.date(2026, 10, 21)), 1, 1, 1, 1, 0)]
    write_msb(tmp_path / "TQQQ.msb", "TQQQ", bars)
    (tmp_path / "manifest.json").write_text(json.dumps({
        "source": "sharadar", "licence": "personal use",
        "tickers": {"TQQQ": {"name": "PROSHARES ULTRAPRO QQQ", "table": "funds", "isdelisted": "N"}},
    }))
    catalog = Catalog(config.Settings(daily_dir=tmp_path))
    coverage = catalog.coverage("tqqq")
    assert (coverage["first_bar"], coverage["last_bar"], coverage["bars"]) == ("2010-02-11", "2026-10-21", 3)
    assert coverage["asset_class"] == "etf" and coverage["licence"] == "personal use"
    assert coverage["synthetic_recipes"] == []
    assert catalog.resolve("tqqq", "2009-12-31") == {
        "security": None, "note": "The data for TQQQ starts on 2010-02-11."}
    assert catalog.resolve("TQQQ", "2015-01-02")["security"]["security_id"] == "tqqq"
    assert catalog.resolve("SQQQ", "2015-01-02")["security"] is None
    assert [s["security_id"] for s in catalog.search("ultrapro")] == ["tqqq"]
    assert catalog.coverage("nope") is None


def test_the_catalog_reads_several_directories_rates_and_validated_recipes(tmp_path):
    from fetch_stooq import closing_bell, write_msb
    import datetime as dt

    prices, rates = tmp_path / "prices", tmp_path / "rates"
    prices.mkdir()
    rates.mkdir()
    bar = lambda day: (closing_bell(day), 1, 1, 1, 1, 0)  # noqa: E731
    write_msb(prices / "TQQQ.msb", "TQQQ", [bar(dt.date(2010, 2, 11)), bar(dt.date(2026, 10, 21))])
    write_msb(prices / "QQQ.TR.msb", "QQQ.TR", [bar(dt.date(1999, 3, 10))])
    write_msb(rates / "DTB3.msb", "DTB3", [bar(dt.date(1954, 1, 4)), bar(dt.date(2026, 10, 20))])
    (rates / "manifest.json").write_text(json.dumps({
        "source": "fred", "terms_url": "https://fred.stlouisfed.org/docs/api/terms_of_use.html",
        "series": {"DTB3": {"title": "3-Month Treasury Bill"}}}))
    recipe = {"record": "synthetic_recipe", "recipe_id": "tqqq_from_qqq_3x", "target_security_id": "tqqq",
              "available_from": "1999-03-10", "validation": {"daily_return_correlation": 0.999}}
    unvalidated = {**recipe, "recipe_id": "unchecked"}
    del unvalidated["validation"]
    (tmp_path / "recipes.json").write_text(json.dumps([recipe, unvalidated]))

    catalog = Catalog(config.Settings(daily_dir=prices, extra_daily_dirs=(rates,),
                                      recipes=tmp_path / "recipes.json"))
    # The total-return file is a series of QQQ's, not a security of its own.
    assert [s["security_id"] for s in catalog.search("")] == ["dtb3", "tqqq"]
    rate = catalog.coverage("dtb3")
    assert (rate["asset_class"], rate["first_bar"], rate["price_adjustment"]) == ("rate", "1954-01-04", None)
    # Only a recipe with a validation is offered.
    assert [r["recipe_id"] for r in catalog.coverage("tqqq")["synthetic_recipes"]] == ["tqqq_from_qqq_3x"]
    note = catalog.resolve("TQQQ", "2006-10-23")["note"]
    assert "starts on 2010-02-11" in note and "tqqq_from_qqq_3x extends it back to 1999-03-10" in note


def test_the_engine_is_given_every_daily_directory_and_the_recipes(tmp_path):
    from app.jobs import JobManager

    (tmp_path / "recipes.json").write_text("[]")
    settings = config.Settings(daily_dir=tmp_path / "a", extra_daily_dirs=(tmp_path / "b",),
                               recipes=tmp_path / "recipes.json")
    command = JobManager(settings, store=None)._command(tmp_path / "run")
    daily = [command[i + 1] for i, arg in enumerate(command) if arg == "--daily"]
    assert daily == [str(tmp_path / "a"), str(tmp_path / "b")]
    assert command[command.index("--recipes") + 1] == str(tmp_path / "recipes.json")
    # No recipes file, no flag: the engine then refuses any synthetic extension by name.
    missing = config.Settings(daily_dir=tmp_path / "a", recipes=tmp_path / "nope.json")
    assert "--recipes" not in JobManager(missing, store=None)._command(tmp_path / "run")


def test_ms_daily_dir_may_name_several_directories(monkeypatch):
    monkeypatch.setenv("MS_DAILY_DIR", "/data/sharadar, /data/fred")
    monkeypatch.setenv("MS_RECIPES", "/data/recipes.json")
    settings = config.from_env()
    assert settings.daily_dirs() == [Path("/data/sharadar"), Path("/data/fred")]
    assert settings.recipes == Path("/data/recipes.json")
