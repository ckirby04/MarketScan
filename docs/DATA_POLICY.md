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

## Why this file exists

A closed door invites a workaround, and the workaround is usually easy. The
Stooq challenge would have taken a dozen lines and the project would have got its
data. Writing the rule down in advance is what makes the answer *policy* rather
than a judgment call made under pressure by whoever is at the keyboard at the
time — including when that is a language model with no stake in the consequences.
