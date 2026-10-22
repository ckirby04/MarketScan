# The research web front end

The Research view as a web app. The home page is a conversation with the assistant (in
[`../service/`](../service/)): a question in plain English, the few follow-ups that would change the
answer, a Run button, and the Results in the order
[`SPEC_RESEARCH_VIEW.md`](../docs/specs/SPEC_RESEARCH_VIEW.md) fixes -- verdict, the gates behind it,
the stats, and only then the assistant's analysis. The Strategy Form
([`contracts/research/strategy_form.schema.json`](../contracts/research/strategy_form.schema.json))
is filled behind the conversation; "Show the form" opens it, and `/form` fills one by hand.

It computes nothing. Every number on the page is a field of the Results the engine produced; the
code here formats and draws. The analysis is the only text written by a model, and is labelled so.

```
browser -> this app (Vercel) -> /api/jobs* (server side, holds the service key) -> job service (Cloud Run)
```

The service's key never reaches the browser: the route handlers in `src/app/api/` attach it.

## Running it locally

```powershell
# the service, in another terminal: see service/README.md
cd web
npm install
$env:RESEARCH_SERVICE_URL = "http://127.0.0.1:8080"
$env:RESEARCH_SERVICE_KEY = "<the service's MS_API_KEY>"
npm run dev
```

`npm run dev` and `npm run build` first copy the schemas from `../contracts/` into
`src/contracts/` (gitignored): one copy of the contracts, read by the engine, the research layer,
the service and this app alike.

## Deploying to Vercel

1. Import the repository in Vercel with the **Root Directory left at the repository root**:
   [`../vercel.json`](../vercel.json) declares this app as the project's one service, `web`, and
   routes every public path to it. The build reads `../contracts/`, so it must be able to see the
   rest of the repository.
2. Set the environment variables:

| variable | value |
|---|---|
| `RESEARCH_SERVICE_URL` | the Cloud Run URL `service/deploy.ps1` printed |
| `RESEARCH_SERVICE_KEY` | `gcloud secrets versions access latest --secret ms-api-key` |
| `WEB_PASSWORD` | any password: the whole app sits behind HTTP Basic auth with it |

A deployment without `WEB_PASSWORD` answers 503 rather than serving the app openly. Locally it
is optional.

## What is here

| | |
|---|---|
| `src/app/page.tsx`, `src/app/c/[id]/page.tsx` | a new conversation, and an existing one |
| `src/components/Chat.tsx` | the conversation: messages, questions as buttons, Run, results, analysis |
| `src/app/form/page.tsx` | the form by hand, and Run |
| `src/app/runs/[id]/page.tsx` | a job's progress, then its Results, or its refusal by form path |
| `src/app/api/jobs/**`, `src/app/api/conversations/**` | the proxy to the service; an assistant turn is streamed through |
| `src/components/FormEditor.tsx` | the schema-driven editor; no widget written for a particular field |
| `src/components/ResultsView.tsx`, `EquityChart.tsx` | the Results, and the one chart |
| `src/lib/schemaForm.ts` | what the editor decides about a schema; the desktop app's `schema_form.cpp` makes the same decisions |
| `src/middleware.ts` | the password |

