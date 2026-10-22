"""The research assistant: a conversation that fills a Strategy Form, runs it,
and explains the Results.

The user sees only the conversation. The model works through the tools in
contracts/research/assistant/tools.json: it looks up the data the service holds
(catalog.py), saves the form as it goes, asks the user questions, and starts the
run when the user asks for it. The engine's Results then come back to it as a
<results> message, and its reply to that is the analysis -- shown below the
verdict and the stats the app renders from the same document, never instead of
them.

A conversation is one JSON document (storage.py) holding:

  messages   the API history, append-only. Nothing in it is ever edited, and the
             system prompt and the tool list are frozen into the document when
             the conversation starts: each thinking block is bound to the exact
             prefix that produced it, and an edited prefix would void it.
  display    what the user sees, in order: their messages, the assistant's,
             questions, the form when it is ready, runs, analyses.
  pending    an ask_user call waiting for its answer, with the results of any
             other tools called in the same message, so the answer and those go
             back together in one user message as the API requires.

A turn is a generator of events, which the HTTP layer streams as NDJSON:

  {"event": "text", "delta": "..."}       assistant text as it arrives
  {"event": "activity", "text": "..."}    what a tool call is doing
  {"event": "item", "item": {...}}        a display item other than text
  {"event": "form", "form": {...}, "ready": bool}
  {"event": "error", "message": "..."}
  {"event": "end"}
"""

from __future__ import annotations

import datetime as dt
import json
import threading
import time
import uuid
from dataclasses import asdict, dataclass, field
from typing import Iterator

import jsonschema

from .catalog import Catalog
from .config import Settings
from .storage import read_json

# list_regimes is in the contract, but there are no named regimes to serve
# until the data catalog (part R1) exists. A tool the model is offered and
# cannot use is worse than one it is not offered.
SERVED_TOOLS = ("search_securities", "resolve", "get_coverage", "save_form", "ask_user", "start_run")

# Model calls in one turn before the service stops it: a loop that has not
# finished in this many is not converging.
MAX_STEPS = 16

BETAS = [
    # A request declined by a safety classifier is re-run on the model
    # Anthropic recommends for that category, instead of failing the turn.
    "server-side-fallback-2026-07-01",
    # Lets the request say what to do with a thinking block whose prefix does
    # not match (below).
    "thinking-binding-controls-2026-08-01",
]

TERMINAL = ("done", "refused", "failed")


@dataclass
class Conversation:
    id: str
    system: str
    tools: list[dict]
    title: str = ""
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)
    messages: list[dict] = field(default_factory=list)
    display: list[dict] = field(default_factory=list)
    form: dict | None = None
    ready: bool = False
    pending: dict | None = None
    runs: list[str] = field(default_factory=list)
    analysed: list[str] = field(default_factory=list)

    def public(self) -> dict:
        """What the web app gets: no API history, no prompt."""
        return {
            "id": self.id, "title": self.title, "created_at": self.created_at,
            "updated_at": self.updated_at, "display": self.display, "form": self.form,
            "ready": self.ready, "runs": self.runs, "analysed": self.analysed,
        }


class Busy(Exception):
    """The conversation is already in a turn."""


class Assistant:
    def __init__(self, settings: Settings, store, jobs, catalog: Catalog, validator, client=None):
        self.settings = settings
        self.store = store
        self.jobs = jobs
        self.catalog = catalog
        self.validator = validator
        if client is None:
            import anthropic

            client = anthropic.Anthropic(api_key=settings.anthropic_api_key)
        self.client = client
        contracts = settings.contracts
        self._prompt = (contracts / "assistant" / "system_prompt.md").read_text(encoding="utf-8")
        self._schema_text = (contracts / "strategy_form.schema.json").read_text(encoding="utf-8")
        self._example_text = (contracts / "assistant" / "example_form.json").read_text(encoding="utf-8")
        tools = json.loads((contracts / "assistant" / "tools.json").read_text(encoding="utf-8"))
        self._tools = [t for t in tools if t["name"] in SERVED_TOOLS]
        self._input_validators = {
            t["name"]: jsonschema.Draft202012Validator(t["input_schema"]) for t in self._tools
        }
        self._locks: dict[str, threading.Lock] = {}
        self._locks_guard = threading.Lock()

    # --- Conversations --------------------------------------------------------

    def create(self) -> Conversation:
        system = (
            self._prompt.replace("{{TODAY}}", dt.date.today().isoformat())
            .replace("{{STRATEGY_FORM_SCHEMA}}", self._schema_text.strip())
            .replace("{{EXAMPLE_FORM}}", self._example_text.strip())
        )
        conversation = Conversation(id=uuid.uuid4().hex[:16], system=system, tools=self._tools)
        self._save(conversation)
        return conversation

    def get(self, conversation_id: str) -> Conversation | None:
        raw = self.store.get_conversation(conversation_id)
        return None if raw is None else Conversation(**json.loads(raw))

    def recent(self, limit: int = 50) -> list[dict]:
        out = []
        for conversation_id in self.store.conversation_ids():
            c = self.get(conversation_id)
            if c is not None and c.display:
                out.append({"id": c.id, "title": c.title, "updated_at": c.updated_at})
        out.sort(key=lambda c: c["updated_at"], reverse=True)
        return out[:limit]

    def _save(self, conversation: Conversation) -> None:
        conversation.updated_at = time.time()
        self.store.put_conversation(conversation.id, json.dumps(asdict(conversation)).encode("utf-8"))

    def _lock(self, conversation_id: str) -> threading.Lock:
        with self._locks_guard:
            return self._locks.setdefault(conversation_id, threading.Lock())

    # --- The three ways a turn starts -----------------------------------------

    def say(self, conversation: Conversation, text: str) -> Iterator[dict]:
        """The user wrote something, or answered the questions."""
        def begin():
            if not conversation.title:
                conversation.title = text.strip().splitlines()[0][:80] if text.strip() else ""
            self._close_questions(conversation)
            item = {"kind": "user", "text": text}
            conversation.display.append(item)
            self._append_user(conversation, text)
            return [{"event": "item", "item": item}]
        return self._turn(conversation, begin, "assistant")

    def run(self, conversation: Conversation) -> dict:
        """The Run button: start the saved form without a model call."""
        lock = self._lock(conversation.id)
        if not lock.acquire(blocking=False):
            raise Busy()
        try:
            item = self._start_run(conversation)
            if "error" in item:
                return item
            self._close_questions(conversation)
            self._append_user(
                conversation,
                f"[The user pressed Run. Run {item['job_id']} has started; its results will follow.]")
            self._save(conversation)
            return item
        finally:
            lock.release()

    def results(self, conversation: Conversation, job_id: str) -> Iterator[dict]:
        """A run of this conversation's has finished: the analysis, or a fix."""
        status = self.jobs.status(job_id)
        if job_id not in conversation.runs or status is None:
            raise KeyError(job_id)
        if status.state not in TERMINAL or job_id in conversation.analysed:
            raise Busy()

        def begin():
            conversation.analysed.append(job_id)
            if status.state == "done":
                results = read_json(self.store, job_id, "results.json")
                text = f"<results job_id=\"{job_id}\">\n{json.dumps(results)}\n</results>"
            else:
                text = (f"<refused job_id=\"{job_id}\" state=\"{status.state}\">\n"
                        f"{json.dumps(status.errors)}\n</refused>")
            self._append_user(conversation, text)
            return []
        return self._turn(conversation, begin, "analysis" if status.state == "done" else "assistant",
                          job_id=job_id)

    # --- The turn --------------------------------------------------------------

    def _turn(self, conversation, begin, kind, job_id=None) -> Iterator[dict]:
        # Taken before the generator is returned, so a second request for the
        # same conversation is refused at once rather than when it is read.
        lock = self._lock(conversation.id)
        if not lock.acquire(blocking=False):
            raise Busy()

        def events():
            try:
                yield from begin()
                self._save(conversation)
                yield from self._steps(conversation, kind, job_id)
            except Exception as error:  # the turn ends; the conversation survives
                message = _describe(error)
                item = {"kind": "error", "text": message}
                conversation.display.append(item)
                yield {"event": "error", "message": message}
            finally:
                self._save(conversation)
                lock.release()
            yield {"event": "end"}
        return events()

    def _steps(self, conversation: Conversation, kind: str, job_id: str | None) -> Iterator[dict]:
        for _ in range(MAX_STEPS):
            parts: list[str] = []
            with self.client.beta.messages.stream(
                model=self.settings.assistant_model,
                max_tokens=64000,
                system=conversation.system,
                tools=conversation.tools,
                messages=conversation.messages,
                thinking={
                    "type": "adaptive",
                    # Normally nothing is dropped: the history is append-only and
                    # the prefix frozen. If a block ever does fail the check, the
                    # turn goes on without it instead of failing.
                    "block_binding": {"prefix_mismatch_behavior": "drop_block"},
                },
                output_config={"effort": self.settings.assistant_effort},
                # The frozen prompt, the tools and the history so far are the
                # same on every call of a conversation: cache them.
                cache_control={"type": "ephemeral"},
                betas=BETAS,
                fallbacks="default",
            ) as stream:
                for event in stream:
                    if event.type == "text":
                        parts.append(event.text)
                        yield {"event": "text", "delta": event.text}
                message = stream.get_final_message()
            _log_usage(conversation.id, message)

            if message.stop_reason == "refusal":
                # Discarded, not kept: a declined turn is not part of the history.
                text = "I can't help with that request."
                conversation.display.append({"kind": "assistant", "text": text})
                yield {"event": "error", "message": text}
                return
            if message.stop_reason == "max_tokens":
                raise RuntimeError("the assistant's reply was cut off at its length limit")

            conversation.messages.append(
                {"role": "assistant", "content": [b.to_dict() for b in message.content]})
            text = "".join(parts).strip()
            if text:
                item = {"kind": kind, "text": text}
                if job_id:
                    item["job_id"] = job_id
                conversation.display.append(item)

            calls = [b for b in message.content if b.type == "tool_use"]
            if not calls:
                return

            results, ask = [], None
            for call in calls:
                if call.name == "ask_user" and self._valid_input(call) is None:
                    ask = call
                    continue
                yield {"event": "activity", "text": _activity(call)}
                content, is_error, emitted = self._call_tool(conversation, call)
                for event in emitted:
                    yield event
                results.append(_tool_result(call.id, content, is_error))

            if ask is not None:
                conversation.pending = {"ask_id": ask.id, "results": results}
                item = {"kind": "questions", "message": ask.input.get("message", ""),
                        "questions": ask.input["questions"], "answered": False}
                conversation.display.append(item)
                yield {"event": "item", "item": item}
                return
            conversation.messages.append({"role": "user", "content": results})
        raise RuntimeError(f"the assistant did not finish within {MAX_STEPS} steps")

    # --- Tools ------------------------------------------------------------------

    def _valid_input(self, call) -> str | None:
        validator = self._input_validators.get(call.name)
        if validator is None:
            return f"there is no tool named {call.name}"
        errors = [e.message for e in validator.iter_errors(call.input)]
        return "; ".join(errors) if errors else None

    def _call_tool(self, conversation: Conversation, call) -> tuple[object, bool, list[dict]]:
        problem = self._valid_input(call)
        if problem:
            return {"error": f"invalid input: {problem}"}, True, []
        args = call.input
        try:
            if call.name == "search_securities":
                return {"securities": self.catalog.search(args["query"], args.get("limit", 10))}, False, []
            if call.name == "resolve":
                return self.catalog.resolve(args["ticker"], args["as_of"]), False, []
            if call.name == "get_coverage":
                coverage = self.catalog.coverage(args["security_id"])
                if coverage is None:
                    return {"error": f"no security with id {args['security_id']}"}, True, []
                return coverage, False, []
            if call.name == "save_form":
                return self._save_form(conversation, args["form"], args["ready"])
            if call.name == "start_run":
                item = self._start_run(conversation)
                if "error" in item:
                    return item, True, []
                return ({"job_id": item["job_id"],
                         "note": "Running. The results will follow in a <results> message."},
                        False, [{"event": "item", "item": item}])
        except Exception as error:  # a broken tool is the model's to hear about
            return {"error": _describe(error)}, True, []
        return {"error": f"there is no tool named {call.name}"}, True, []

    def _save_form(self, conversation: Conversation, form: dict, ready: bool):
        conversation.form = form
        conversation.ready = False
        if not ready:
            return {"saved": True}, False, [{"event": "form", "form": form, "ready": False}]
        errors = [
            {"path": "/" + "/".join(str(p) for p in e.absolute_path), "message": e.message}
            for e in self.validator.iter_errors(form)
        ]
        for path, entry in (form.get("provenance") or {}).items():
            if isinstance(entry, dict) and entry.get("source") == "missing":
                errors.append({"path": path, "message": "still marked missing: ask, or resolve it"})
        if errors:
            return ({"accepted": False, "errors": errors}, False,
                    [{"event": "form", "form": form, "ready": False}])
        conversation.ready = True
        item = {"kind": "ready", "form": form}
        conversation.display.append(item)
        return ({"accepted": True}, False,
                [{"event": "form", "form": form, "ready": True}, {"event": "item", "item": item}])

    def _start_run(self, conversation: Conversation) -> dict:
        if conversation.form is None or not conversation.ready:
            return {"error": "there is no accepted form to run: save it with ready=true first"}
        status = self.jobs.submit(conversation.form)
        conversation.runs.append(status.job_id)
        item = {"kind": "run", "job_id": status.job_id}
        conversation.display.append(item)
        return item

    # --- History ----------------------------------------------------------------

    def _append_user(self, conversation: Conversation, text: str) -> None:
        """A user message; if questions are waiting, this is their answer."""
        if conversation.pending:
            pending, conversation.pending = conversation.pending, None
            content = [*pending["results"], _tool_result(pending["ask_id"], text, False)]
        else:
            content = [{"type": "text", "text": text}]
        conversation.messages.append({"role": "user", "content": content})

    @staticmethod
    def _close_questions(conversation: Conversation) -> None:
        for item in reversed(conversation.display):
            if item["kind"] == "questions":
                item["answered"] = True
                break


def _tool_result(tool_use_id: str, content, is_error: bool) -> dict:
    text = content if isinstance(content, str) else json.dumps(content)
    result = {"type": "tool_result", "tool_use_id": tool_use_id, "content": text}
    if is_error:
        result["is_error"] = True
    return result


def _activity(call) -> str:
    # Shown to the user while the tool runs: plain words, no tool or form names.
    args = call.input if isinstance(call.input, dict) else {}
    match call.name:
        case "search_securities":
            return f"Looking up {args['query']}" if args.get("query") else "Looking at the data available"
        case "resolve":
            return f"Checking {args['ticker']}" if args.get("ticker") else "Checking the ticker"
        case "get_coverage":
            return "Checking how far back the prices go"
        case "save_form":
            return "Checking the setup" if args.get("ready") else "Setting up the backtest"
        case "start_run":
            return "Starting the backtest"
    return "Working"


def _describe(error: Exception) -> str:
    message = getattr(error, "message", None) or str(error) or type(error).__name__
    return f"{type(error).__name__}: {message}"


def _log_usage(conversation_id: str, message) -> None:
    # One line per model call in the service's log: what a conversation costs.
    usage = getattr(message, "usage", None)
    if usage is None:
        return
    print(json.dumps({
        "assistant_call": conversation_id,
        "model": getattr(message, "model", ""),
        "stop": message.stop_reason,
        "input": getattr(usage, "input_tokens", None),
        "cache_read": getattr(usage, "cache_read_input_tokens", None),
        "cache_write": getattr(usage, "cache_creation_input_tokens", None),
        "output": getattr(usage, "output_tokens", None),
    }), flush=True)
