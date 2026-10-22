# Data access policy

Standing rules for how this project acquires data. Policy, not a decision to be
re-made each session: the point of writing it down is that the next person to hit
a closed door does not get to reason their way through it.

---

## 1. No circumvention

**Nothing in this repository defeats an access control on a data source.** Not a
proof-of-work challenge, not bot detection, not a CAPTCHA, not a rate limit
evaded by rotating identifiers, not a source whose terms forbid scraping.

**Triviality is not a justification, and this is the rule most likely to be
argued with.** A gate that takes milliseconds to pass is still a gate. The
difficulty of a control is a fact about its implementation; whether passing it is
acceptable is a fact about what the operator was trying to say. Those are
different questions and the first one is not evidence about the second.

### The worked example: Stooq

Stooq was this project's only price source. As of 2026-09-17 its free CSV
endpoint answers every request with **HTTP 200 and a proof-of-work challenge** —
find `n` such that `SHA-256(c + n)` begins with four hex zeros, then `POST` the
answer to `/__verify`.

Diagnosed from this machine on 2026-09-21:

| | |
|---|---|
| DNS | resolves to Hetzner, Stooq's own hosting |
| TCP + TLS | completes, TLS 1.3 |
| HTTP | 200, `Server: openresty`, `Cache-Control: no-store`, 796 bytes |
| Rate-limit signals | none — no `429`, no `Retry-After` |
| CDN | no `CF-RAY`; Stooq's own edge, not Cloudflare |

Served **unconditionally on a first request from a cold connection**, which is
what distinguishes a deliberate gate from throttling: a rate limiter does not
challenge you before you have asked for anything.

**Four hex zeros is about sixty-five thousand SHA-256 hashes — a few
milliseconds, and perhaps a dozen lines.** That is recorded here precisely
because it made no difference to the decision. A challenge served unconditionally
is the operator saying automated access is not welcome on these terms, and that
answer is taken at face value. Stooq is excluded (§5), and the milestone that
depended on it waits for a source that grants access rather than one that can be
out-computed.

## 2. Diagnosis is permitted, persistence is not

One request to characterise a failure is fine, and is how the table above was
produced — a failure nobody has diagnosed gets described wrongly, and this one
had been recorded as "a JavaScript page" and "possibly temporary", both of which
turned out to be understated.

A second request **shaped to get past** the control is not. The line is intent,
not count: reading a challenge to find out what it is differs from answering it,
and anyone can tell which one they are doing.

## 3. A source must grant programmatic access in its own terms

The bar is **an API key and documented terms**. A source that publishes terms
permitting automated access, and a credential to identify the client, is usable.

**Silence is not consent.** The absence of an explicit prohibition is not
permission, and neither is the technical fact that a request succeeds. Whether a
door is unlocked is not the question.

## 4. Nothing sensitive in the repository, and nothing licensed

No API keys, no credentials, no `.env`, no market data. `data/cache/` is
gitignored and stays that way.

### Licensed data is never committed and never redistributed

This is the rule that matters most, and it is written now, while it is
hypothetical, so that it is already law by the time it is not.

CRSP and Compustat are licensed **to the institution, not to the researcher**.
Committing an extract to a public GitHub repository is a licence violation, and
universities treat it seriously — it is the kind of thing that ends access for
everyone at the institution, not just the person who did it.

If WRDS access materialises:

- **Extracts live outside the repository entirely.** Not in a gitignored folder
  inside it — outside it. A gitignore is a convenience that one `git add -f` or
  one misconfigured tool defeats.
- **The cache path stays gitignored** regardless, as a second line rather than a
  first.
- **Derived results published here must be aggregate rather than
  reconstructible.** A coefficient, a Sharpe, a count of gaps: fine. A per-security
  table from which the underlying series could be rebuilt: not fine, however
  aggregated it looks. The test is whether someone could recover the vendor's data
  from what is published, not whether the file is called a result.
- **Nothing licensed goes into a fixture**, however small, however synthetic it
  is made to look.

## 5. Currently approved sources

| Source | Used for | Basis |
|---|---|---|
| [`fja05680/sp500`](https://github.com/fja05680/sp500) | S&P 500 membership history | Public repository, open licence, fetched through the GitHub API. Read via `scripts/fetch_sp500.py`. |
| `research/panel.py` | Everything else | Synthetic. Generated here, owned here, committed as fixtures. Not market data and labelled as such everywhere it appears. |
| [Sharadar](https://sharadar.com) | Daily prices for US stocks and funds, listed and delisted | API key and documented personal-use terms ([sharadar.com/terms](https://sharadar.com/terms)), meeting §3; delisted securities in the same tables, meeting §6's binding constraint. Fetched by `scripts/fetch_sharadar.py`, which takes the key from `SHARADAR_API_KEY` only, sends it as a header, slices requests by ticker and date, and refuses a cache path inside the repository. **Licensed:** the cache lives outside the repository (§4), nothing from it is committed or redistributed, and what is published from it is aggregate. The free key covers about five years of a sample of large US stocks and no funds; a paid Prices plan covers funds and, at the full-history tier, from 1997. |

### Excluded

| Source | Reason |
|---|---|
| **Stooq** | Proof-of-work challenge served unconditionally (§1). Excluded until it grants programmatic access in its own terms. `scripts/fetch_stooq.py` is retained because it detects and reports the gate correctly; it does not attempt to pass it. |

## 6. Candidate sources, if a price vendor is needed before CRSP

Both grant API access explicitly, which is the bar in §3:

| | |
|---|---|
| **Tiingo** | Free tier, API key, documented terms. |
| **EODHD** | Paid, API key, documented terms. |

**The caveat that governs the choice: delisted coverage is the binding
constraint.** The milestone that needs a price vendor — M3b, measuring price
continuity across a membership gap — is asking about securities that may have
stopped trading decades ago. A vendor carrying only currently-listed tickers
cannot answer that question **at any price**, and paying for one would buy
coverage of exactly the names the question is not about.

So the question to ask a vendor is not "how much history" but "do you carry names
that are no longer listed, and with what survivorship guarantee". If the answer is
no, the vendor does not solve this problem regardless of its price, and the
milestone stays CRSP-dependent — see
[`STATUS.md`](STATUS.md) for why that reclassification happened.

---

## 7. Broker access

Everything above governs market data. It governs a broker identically, and a
broker adds two rules of its own, because a mistake here moves money.

### Paper only, and structurally rather than by configuration

**The live-money trading host does not appear anywhere in this repository.** Not
in a constant, not in a comment, not in a config example, not behind a flag.
There is no value any setting could take that reaches a live account, because the
string is not present to be reached.

That is deliberately stronger than a safe default. A default can be changed by
somebody who does not know what it protects — including a future session of an
assistant reading a config key and helpfully making it configurable. An absent
string cannot.

Enforced in three places:

| | |
|---|---|
| `scripts/check_no_live_endpoints.py` | Scans the source tree. Runs in CI before anything is built. |
| `tests/test_paper_only.cpp` | Scans the **built binaries**, which a source grep cannot: a string can reach a binary through a header, a generated file or a dependency without appearing in any source file a reviewer would read. |
| `HttpClient` and `WebSocket` constructors | Throw on a host that is not on the allowlist. The only one of the three that acts at *runtime*: a hostname assembled from pieces at run time passes both scans, and fails here. |

The third was not theoretical. `ms_live guard` demonstrates it by building a live hostname at runtime from a prefix and the broker domain, and watching the constructor refuse it. An earlier draft of that command spelled the live host out as a literal so the refusal would be legible — and `check_no_live_endpoints.py` failed the tree, which is how the first guard was observed catching the second author of a violation rather than the first.

**Both check an allowlist, not a blocklist**, and the reason is worth stating
because the obvious implementation is wrong. The live trading host is the paper
host with a prefix removed, so searching for the live host *also matches the
paper host*, and searching for it while excluding the paper host misses anything
spelled slightly differently. So neither guard looks for a forbidden string.
Each finds every host in the broker's domain and requires it to be one that is
allowed — which fails on an endpoint nobody anticipated, and which means the
forbidden hostname never has to be written down. A guard that names the thing it
forbids has put the thing it forbids in the repository.

### Credentials come from the environment and nowhere else

No file, no config key, no command-line argument, no default. `APCA_API_KEY_ID`
and `APCA_API_SECRET_KEY`, read at the point of use.

- **Absent credentials are an error, not a fallback.** A missing key names the
  variable that is missing. It does not quietly fall back to a simulator, a
  read-only mode, or a different account — a run that looks live, reports
  numbers, and was never connected to anything is the same class of bug as a test
  that passes without measuring.
- **A secret is never logged, printed or put in an error message.** `redact()`
  returns the first four characters of a key id and nothing else: enough to tell
  two accounts apart in a log, not enough to be one.
- **The gitignore entries were added before the first key existed**, which is the
  only time adding them is free.
- `scripts/check_no_secrets.py` fails on anything shaped like a key or secret
  beside a credential word, because a gitignore is a convenience that one
  `git add -f` defeats.

Both scanners have been run against a deliberate violation and observed to fail.
A guard that has only ever seen clean input is an assertion that has never been
tested — the same rule the rest of this project's tests are held to.

### Rate limits are respected, not worked around

§1 and §2 apply to a broker exactly as they apply to a data vendor. A rate limit
is backed off from and logged. It is not evaded by rotating identifiers, opening
parallel connections, or retrying faster. If a limit makes something impossible,
that is a finding to report, not an obstacle to route around.

### Recorded market data is never committed

Checked before recording anything, rather than after. Alpaca's disclosures library
links the **NASDAQ OMX Global Subscriber Agreement** and the **NYSE Market Data
Display Services Agreement** as the agreements governing market data received
through it. The NASDAQ one says a subscriber:

> "may not sell, lease, **furnish** or otherwise permit or **provide access to the
> Information to any other Person** or to any other office or place"

and, for a non-professional subscriber, that "the Information is licensed **only
for personal use**."

Committing a recorded session to a public repository furnishes it and provides
access to any other Person. So it is not done, and this is not a close call.

The free tier is IEX data, which is a different exchange from the two those
agreements name — and nothing in Alpaca's terms clearly grants redistribution of
it either. §3 settles that: **silence is not consent.**

**What this costs and how it is handled.** The live/replay equivalence test
cannot ship a real recording as a fixture, so it does not have one. Instead:

- `ms_live equivalence` records a session, replays it, compares, and then **deletes
  the recording**. `--keep` exists for debugging a divergence and says on the console
  that what it kept must not be committed.
- `.gitignore` covers `*.ndjson`, `recordings/` and `data/recordings/` as well as
  `data/cache/`, so a recording written into the repository to look at cannot be
  committed by accident.
- The committed tests use messages **hand-written to the vendor's documented shapes
  with invented prices**. Those are not market data by any reading, and they are what
  lets the parser, the recorder round trip and the whole order lifecycle be tested in
  CI with no network and no fixture.

A real session is recorded, compared and then thrown away, and **the result of that
comparison is reported** — a measurement is not redistribution, and a statement that
two runs agreed on ten decisions carries none of the underlying data. The results are
in [`EXECUTION.md`](EXECUTION.md) with their dates.

### What paper trading is not

Paper fills are the venue's simulation, not the venue's book. They are better
than a local fill simulator for exercising the *order lifecycle* — rejections,
partials, cancels that lose a race — and they are **not** evidence about fill
quality, queue position, or what an order would have done to a real book.

Nothing measured against paper is a statement about execution.

---

## Why this file exists

A closed door invites a workaround, and the workaround is usually easy. The
Stooq challenge would have taken a dozen lines and the project would have got its
data. Writing the rule down in advance is what makes the answer *policy* rather
than a judgment call made under pressure by whoever is at the keyboard at the
time — including when that is a language model with no stake in the consequences.
