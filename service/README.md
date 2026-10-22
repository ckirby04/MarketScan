# The research job service

`ms_research` behind HTTP, for the web front end in [`../web/`](../web/). A Strategy Form in, a job
id out; then status, and when it is done the Results document and the chart curves. The contracts
in [`../contracts/research/`](../contracts/research/) are the API: the request body is a
`strategy_form.schema.json` document and the results are a `results.schema.json` one.

```
POST /jobs                   form in -> 202 {job_id, state: "queued"}  (422 with errors if invalid)
GET  /jobs                   recent jobs
GET  /jobs/{id}              queued | running (stage, done, total) | done (grade) | refused | failed
GET  /jobs/{id}/results      results.json
GET  /jobs/{id}/curves       curves.json
GET  /jobs/{id}/form         the form as run
POST /jobs/{id}/cancel       stop it; no partial Results are kept
GET  /contracts/form-schema  for a form editor
GET  /health                 no auth
```

Every route but `/health` needs `Authorization: Bearer $MS_API_KEY`. The key never reaches a
browser: the web front end calls the service from its own server.

## How it runs a job

It validates the form with the research layer's validator ([`research/contracts.py`](../research/contracts.py)),
then starts `ms_research --machine`, which prints one JSON event per line -- progress, then `done`
or `refused` -- and writes the run directory `research::write_run` lays out. The service turns the
events into a status, and when the job ends copies the run to a Cloud Storage bucket, because a
Cloud Run instance's disk goes when the instance does.

The engine is unchanged by any of this: the same binary, the same Results, as the desktop app and
the committed cross-test fixtures.

## Running it locally

```powershell
pip install -r service/requirements-dev.txt
$env:MS_API_KEY = "dev"
$env:MS_RESEARCH_BIN = "out/build/ci/bin/ms_research.exe"
cd service
uvicorn app.main:create_app --factory --port 8080
```

With no `MS_DAILY_DIR` or `MS_CSV`, the committed synthetic series is offered as `FIXTURE`, so the
starter form runs before any real data is attached.

| variable | meaning |
|---|---|
| `MS_API_KEY` | the bearer key; empty refuses every request |
| `MS_RESEARCH_BIN` | the engine; `/usr/local/bin/ms_research` in the image |
| `MS_CONTRACTS` | the contract schemas |
| `MS_DAILY_DIR` | a directory of `SYMBOL.msb` daily files |
| `MS_CSV` | `SYMBOL=path,...` date,close CSVs |
| `MS_RUNS_DIR` | where runs are written; `/tmp/runs` |
| `MS_RUNS_BUCKET` | the bucket runs are copied to; empty keeps them local |
| `MS_NULL_DRAWS`, `MS_BOOTSTRAP_PATHS` | JOB.md's 1,000 each by default |

## Deploying to Cloud Run

```powershell
service\deploy.ps1 -Project <your-gcp-project>
```

It builds the image on Cloud Build (no local Docker needed), creates the bucket and an API-key
secret the first time, and deploys. The script's header says why each Cloud Run setting is what it
is; the short version is one instance, CPU kept on between requests, scale to zero when idle.

Two known limits, both acceptable for one user and both to revisit before more:

- Job status is in the instance's memory while a job runs, and reaches the bucket when it ends.
  If Cloud Run stops the instance mid-job, that job is lost and its id returns 404.
- Real data: `MS_DAILY_DIR` can point at a Cloud Storage bucket mounted as a volume. Which vendor
  fills it is open; see the comparison in the session notes, and `docs/DATA_POLICY.md`.

## Tests

```powershell
python -m pytest service/tests -q
```

They run the service against `tests/fake_ms_research.py`, which speaks the same `--machine`
protocol, so they need no engine build.
