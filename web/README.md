# The research web front end

The Research view as a web app: a Strategy Form drawn from
[`contracts/research/strategy_form.schema.json`](../contracts/research/strategy_form.schema.json),
submitted to the job service in [`../service/`](../service/), and the Results document shown in the
order [`SPEC_RESEARCH_VIEW.md`](../docs/specs/SPEC_RESEARCH_VIEW.md) fixes -- verdict, the gates behind
it, the stats, and only then analysis.

It computes nothing. Every number on the page is a field of the Results the engine produced; the
code here formats and draws.

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

1. Import the repository in Vercel, and set the project's **Root Directory** to `web`. Leave
   *Include files outside the root directory in the Build Step* on: the build reads
   `../contracts/`.
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
| `src/app/page.tsx` | the form, and Run |
| `src/app/runs/[id]/page.tsx` | a job's progress, then its Results, or its refusal by form path |
| `src/app/api/jobs/**` | the proxy to the service |
| `src/components/FormEditor.tsx` | the schema-driven editor; no widget written for a particular field |
| `src/components/ResultsView.tsx`, `EquityChart.tsx` | the Results, and the one chart |
| `src/lib/schemaForm.ts` | what the editor decides about a schema; the desktop app's `schema_form.cpp` makes the same decisions |
| `src/middleware.ts` | the password |

Not built: the assistant (part R2). The Ask box on the left says so.
