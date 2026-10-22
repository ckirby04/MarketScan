# Handoff: where the web product stands

Written 2026-10-22 to end one Claude Code session and start the next. Read this first, then
[`STATUS.md`](STATUS.md) (the long-form record) and [`PROTECTED.md`](PROTECTED.md) before
removing anything.

## What MarketScan is now

A plain-English trading question goes in ("what's the optimal drawdown to buy TQQQ over the past
20 years?"). An assistant turns it into a Strategy Form by conversation, the C++ engine runs the
backtest with its robustness checks, and the user gets back results in a fixed order:
**verdict, then reasoning, then stats, then the assistant's analysis**. The order is deliberate:
analysis, however careful, can mislead, so it comes last.

| Piece | Where | State |
|---|---|---|
| Engine + research job | `src/marketscan/research/`, `apps/research/ms_research.cpp` | done, cross-tested against `research/` (Python) to exact agreement |
| Desktop Research view (R4) | GUI | done |
| Job service + assistant | `service/` (FastAPI) on Cloud Run, `marketscan-research` in project `marketscan-512402`, us-central1 | deployed at commit `56efecf` |
| Web app | `web/` (Next.js) on Vercel | deployed from `main`; home page is the chat, the old form is at `/form` |
| Price data | Sharadar, licensed, in `G:\Quant\marketscan-data\sharadar` and `gs://marketscan-512402-marketscan-data/sharadar/` | never in the repo |
| Rates | FRED DTB3 in `G:\Quant\marketscan-data\fred` and the bucket's `fred/` | |
| Synthetic recipes | `G:\Quant\marketscan-data\recipes.json` and the bucket root | one recipe: `tqqq_from_qqq_3x` |

Service URL: `https://marketscan-research-7xbngotfiq-uc.a.run.app` (`/health` needs no key).

## Git

- Work happens on branch **`research-view`**; the user merges to `main` through PRs on
  `github.com/ckirby04/MarketScan`. Don't merge to `main` yourself.
- `main` is at the merge of PR #4 (`5525a24`).
- **Not yet merged:** `ba45b81` (synthetic history) and `56efecf` (deploy env-vars file). The
  user was about to open PR #5. Nothing in `web/` changed in them.
- CI has not been run on any of the research-view work; everything is green locally.

## The last two features, in brief

**The assistant (R2).** `service/app/assistant.py`. Claude (`claude-opus-5-5`, adaptive thinking,
effort `medium`, server-side fallback) gets `contracts/research/assistant/system_prompt.md` and
`tools.json`: `search_securities`, `resolve`, `get_coverage` (answered by `service/app/catalog.py`
from the files on disk), `save_form`, `ask_user`, `start_run`. Conversations are append-only with
the system prompt and tools frozen per conversation, so preserved thinking stays valid. They're stored
in the runs bucket under `conversations/`. On Opus 5.5, text written between tool calls becomes
thinking and never reaches the user, which is why `ask_user` carries a `message` field. Web side:
`web/src/components/Chat.tsx`, `ChatPage.tsx`, `app/api/conversations/**` (NDJSON passed
through, `maxDuration = 300`).

**Synthetic history (JOB.md §1a).** A leveraged fund extended back before its first real bar
from its underlying's total return, financed at the 3-month T-bill:
`r_k = L*(U_k/U_(k-1) - 1) - (fee + (L-1)*(rate+spread))/100/252`, built backwards from the first
real close. Implemented twice and cross-tested: C++ in `job.cpp` and Python in
`research/synthetic.py`. The engine honours a form's `synthetic_extension` only for a recipe in
`--recipes` whose validation `scripts/build_recipes.py` measured. TQQQ from QQQ: L=3, fee 0.95%,
spread 0.5%. Over 2010-02-11 to 2026-10-21 it has a daily correlation of 0.9989 and tracks
+0.53% a year. Available from 1999-03-10. Verified on Cloud Run: the example form
(`contracts/research/assistant/example_form.json`) gives full-period CAGR 27.900344552247635,
identical to the local run.

## Open next steps (none started)

1. **The user clicks through the web chat once.** Never verified in a browser, because the Chrome
   extension is signed in to another account. Everything was tested through the API instead.
2. **Cash yield `tbill_3m`.** Cash earns 0% today, and every analysis lists that as a bias against
   waiting strategies. DTB3 is already loaded. This is engine accounting plus the matching Python,
   cross-tested, plus FORM/JOB docs and the system prompt (`cash.yield` currently tells the model
   it's unsupported).
3. **More recipes**, such as UPRO from SPY or SOXL from SOXX (neither is held yet): fetch, add to
   `build_recipes.py`, check the validation, upload.
4. **The data catalog (R1)**: ticker renames, delistings, gaps. `catalog.py` is a stand-in.
5. Run CI on a PR. Regenerate the old free Sharadar key.

## How to do things

```powershell
# build and test (Norton: keep -j 3)
.\scripts\dev-env.ps1 cmake --build out/build/ci -j 3
.\scripts\dev-env.ps1 python scripts/check_skips.py --preset ci
python -m pytest research/tests -q
python -m pytest service/tests -q

# after adding or removing tests, and after touching docs
python scripts/check_test_manifest.py --preset ci --write
python scripts/check_test_manifest.py --source pytest --write
python scripts/check_doc_claims.py --preset ci          # --write to refresh counts
python scripts/check_no_secrets.py

# data
python scripts/fetch_sharadar.py ...      # SHARADAR_API_KEY
python scripts/fetch_fred.py              # FRED_API_KEY
python scripts/build_recipes.py           # rerun whenever prices or rates are refreshed
# then upload: commands in service/README.md

# redeploy the service (gcloud.cmd lives in %LOCALAPPDATA%\Google\Cloud SDK\google-cloud-sdk\bin)
service\deploy.ps1 -Project marketscan-512402 -DataBucket marketscan-512402-marketscan-data

# run the service locally
$env:MS_API_KEY = "dev"; $env:MS_RESEARCH_BIN = "out/build/ci/bin/ms_research.exe"
$env:MS_DAILY_DIR = "G:\Quant\marketscan-data\sharadar,G:\Quant\marketscan-data\fred"
$env:MS_RECIPES = "G:\Quant\marketscan-data\recipes.json"
cd service; uvicorn app.main:create_app --factory --port 8080
```

## Rules that bind every session

- **Secrets:** API keys come only from user environment variables (`SHARADAR_API_KEY`,
  `FRED_API_KEY`, `ANTHROPIC_API_KEY`). Read them with
  `[Environment]::GetEnvironmentVariable("X","User")` and never print them, write them to a repo
  file, or ask the user to paste one. The same goes for the `ms-api-key` and `anthropic-api-key`
  secrets.
- **Licensed data** (Sharadar, and recipes measured from it) stays in `G:\Quant\marketscan-data`
  and the private bucket. It's personal use and never redistributed (`DATA_POLICY.md` §5).
- **Deliberate duplication:** C++ and Python implement the same rule independently and are compared
  exactly. Never give them a shared core.
- **Vacuous-pass law:** every new test gets mutation-checked. Break the code, watch the test fail,
  then restore it.
- **Dates and data facts** come from the files, never from memory.
- **This machine:**
  - The working copy is CRLF.
  - Bash heredocs eat backslashes, so use Edit/Write.
  - Python is `python`, not `python3`.
  - Exes need their bin dir (`fmt.dll`, `spdlog.dll`).
  - Norton holds fresh binaries.
  - On Windows, `fromtimestamp` fails for dates before 1970.
  - `gcloud.cmd` arguments pass through cmd.exe, which mangles `|`, `^` and `,`.
