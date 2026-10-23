# The desktop chat

Draft for review, 2026-10-22. Nothing here is built yet.

The desktop Research view gets the web app's conversation: a question in plain English, the
assistant's follow-up questions as buttons, Run, the Results, then the analysis. It does this by
talking to the job service the web app already uses, not by running an assistant of its own.

## 1. The decision: through the service, not in process

[`SPEC_RESEARCH_VIEW.md`](SPEC_RESEARCH_VIEW.md) §5.2 planned an assistant worker inside the
desktop app: its own tool loop against the model API, catalog tools answered locally, runs on this
machine. That was written before the assistant existed. It has since been built in the service
(`service/app/assistant.py`), and the web app is its first client. This document proposes making
the desktop app its second client instead of building the assistant again. The spec files are
records and are not edited, so this document is where the change is written down.

| | Through the service (proposed) | In process (§5.2 as written) |
|---|---|---|
| The assistant | One implementation, the service's | A second one in C++: tool loop, streaming, conversation storage, preserved thinking |
| Conversations | Shared: a question asked on the web continues on the desktop | Desktop-only |
| Where chat runs execute | The service (Cloud Run), on the bucket's copy of the data | This machine, on the local data |
| Email alerts | Fire for chat runs, as on the web | Never (alerts live in the service) |
| Needs the network | For the chat. Hand-filled forms still run locally | For the model API |
| Key on this machine | The service's key | The Anthropic key |
| Size of the work | A client, a worker, a view | All of that plus the assistant |

**Recommended: through the service.** The cost is that a chat run executes in the cloud. That
copy of the data is the same Sharadar files, uploaded from this machine. Forms filled in by hand
keep running locally, unchanged.

## 2. What the user sees

The Research view's rail gains a **Conversations** list, the same one the web shows, above
**Recent runs** (local runs, as now). The main area shows one of:

- **A conversation:** user messages as tinted bubbles on the right, the assistant's text on the
  left, questions as buttons (one question answers on a click; several get "Continue"),
  "Run the backtest" after the assistant's explanation, a run card with progress, then the
  Results through the existing renderer, then the analysis under its AI label. A composer at the
  bottom: Enter sends, Shift+Enter is a new line. The view follows new text only while the
  reader is at the bottom, and scrolls to the verdict when Results arrive, as the web does.
- **The form**, as now, reached by "Fill in a form by hand". Its runs are local.
- **A local run's Results**, as now.

The form never appears in a conversation, as on the web. "Edit form" on a chat run's Results
opens that run's form by hand, as a local run.

When the chat cannot work (no key, the service unreachable or refusing the key), the Ask box
says which, in one sentence, and the form still works. That is SPEC_RESEARCH_VIEW §8's "with no
API key, the view says why the assistant is off, and a hand-filled form runs", kept.

## 3. The network client

### Not `live/http.hpp`

SPEC_RESEARCH_VIEW contradicts itself here. §5.3 says to reuse `live/http.hpp`, and §2 says
nothing in the research path links against `live/`. §2 is the rule that protects money, so it
wins. The broker client also cannot do this job as it is: it reads whole bodies (a turn streams
for up to five minutes), takes broker credentials, and refuses every host but the paper ones.

### A new client: `research::ServiceClient`

`include/marketscan/research/service_client.hpp`, in the research library, with WinHTTP like the
broker client (no new dependency). About 300 lines, written fresh rather than extracted from
`live/http.cpp`: extracting a shared core would mean changing the broker path's code, and the
research client needs streaming the broker one does not.

- **Its own allowlist,** `include/marketscan/research/endpoints.hpp`: the deployed service's
  host, and loopback (`127.0.0.1`, `localhost`) for running the service locally. The constructor
  refuses any other host, as the broker client does with its list. The URL comes from
  `MS_SERVICE_URL` and defaults to the deployed service.
- **TLS required,** except to loopback.
- **The key** comes from the user environment variable `MS_SERVICE_KEY`, read with the hygiene
  of `live/credentials.hpp`: never logged, never in an error, never in a snapshot.
- **Requests:** `get_json`, `post_json`, and `post_stream`, which calls back once per NDJSON line
  as it arrives and can be cancelled. Timeouts: 30 seconds for ordinary requests, 300 for a turn
  (the web route's `maxDuration`).
- **429** is returned with its Retry-After and honoured, never retried around (DATA_POLICY).
- **Behind an interface,** `ServiceTransport`, so every test uses a fake and none touches the
  network (SPEC_RESEARCH_VIEW §8).

### Tests the lists need

- The research list and the broker list are disjoint, and the research list holds nothing in the
  broker's domain. Extends `tests/test_paper_only.cpp` and `scripts/check_no_live_endpoints.py`
  (both already scan for unknown broker hosts).
- `ServiceClient` refuses a host not on its list, and a broker host in particular.
- The research library still does not link `live/`. This is a CMake dependency check.

## 4. The session

`ResearchSession` gains a **chat worker**, a thread beside the job worker, with the same
discipline: the GUI posts commands, the worker publishes snapshots, nothing on the GUI thread
waits.

Commands: `list_conversations`, `open_conversation(id)`, `new_conversation`, `send(text)`,
`run()`, and, internally, `analyse(job_id)` when a chat run ends.

`ResearchSnapshot` gains:

- `chat_available` and `chat_reason`, replacing `assistant_available` and `assistant_reason`;
- `conversations` (id, title, updated);
- the open conversation: its items (user, assistant, analysis, questions, ready, run, error)
  as the service's `display` gives them, the live text growing as deltas arrive, the activity
  line, busy, and the last problem;
- for each chat run: its status, polled once a second while it runs, then its Results and
  curves, fetched from `/jobs/{id}/results` and `/curves`, checked against
  `results.schema.json`, and read by the existing `read_results`. The renderer is not forked.

The event handling mirrors the web's `Chat.tsx`: text deltas grow the live item, an `item` event
closes it, the service's copy of a sent message replaces the one shown at once, and the
conversation is reloaded from the service when a turn ends, so what is on screen after a turn is
the service's record.

## 5. Drawing it

- **Markdown:** the assistant writes short paragraphs, **bold**, bullet and numbered lists, and
  small tables (system prompt, "Style"). A renderer for exactly that subset, in a function that
  turns text into blocks (testable without ImGui) and a drawer.
- **Bold** needs a bold face. The theme loads only `Inter-Regular.ttf`, so this adds
  `Inter-SemiBold.ttf` (same family, same SIL Open Font License). Without it, bold would be
  brighter ink only.
- **Bubbles, chips, cards** reuse the helpers from the visual refresh.

## 6. What is needed before building

1. **This decision,** and the departure from SPEC_RESEARCH_VIEW §5.2 and §5.3 it makes.
2. **The service key on this machine, set by you.** `MS_SERVICE_KEY` as a user environment variable,
   holding the value of the `ms-api-key` secret. A session never reads or prints that secret.
3. **The deployed service's URL fixed in the allowlist.** The Cloud Run hostname is stable while
   the service keeps its name and region. A custom domain later would be one line in the list and
   one in the host test.
4. **Merges, in order:** PR #6 (`research-view`), then `scorecard`, `web-look`, `desktop-port` and
   `desktop-look`. The chat builds on `desktop-look`. Nothing in the service needs to change. Its
   latest work (plain status lines, alerts) needs a redeploy for the chat to show it.
5. **Inter SemiBold** added to `assets/fonts/`, with its licence.

## 7. Build order

1. `ServiceClient`, the research allowlist, and the list tests. No UI.
2. The chat worker and snapshot, tested against a fake transport replaying recorded NDJSON
   turns (a question card, a run, an analysis).
3. The view: conversations list, transcript, markdown, questions, Run, composer, follow-scroll.
4. Chat runs: status polling, Results and curves through the existing renderer, the analysis turn.
5. Screenshots of a conversation and its Results, README and STATUS.

Steps 1–2 are testable without a window, and 3–4 are checked by screenshots as the visual refresh
was. A screenshot of a live conversation needs the service. It can run against a local service
(`MS_SERVICE_URL=http://127.0.0.1:8080`) so no screenshot depends on the deployment.

## 8. Not in this

- Running a chat's form locally instead of in the cloud. Possible later as a "run here" choice,
  but the service would then never see the Results, so no analysis and no alert.
- The data catalog (R1), Open in Operator, and scheduled scans, which are separate work.
