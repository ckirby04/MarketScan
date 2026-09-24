# The result was wrong, and finding that out was the point

**MarketScan** is a quantitative research platform — a C++20 event engine, a
plugin ABI, a Python research layer — built over nine sessions. This is an account
of what it produced, which is not what it was built to produce.

---

## 1. The finding

A strategy in this repository measured a **Sharpe ratio of 2.414**.

That number means nothing on its own, so the platform had already measured what it
would have to beat. A sweep of **1,512 null runs** — the same pipeline, the same
search, over data with no signal in it — established that a search of 252 variants
over three years produces a best-of-search Sharpe of **1.927** by chance alone.
2.414 cleared it.

**The entire result was lookahead.** Every fill the strategy made — 56 of 56 on
one fixture, 55 of 55 on the other — had taken its price from the very bar whose
data triggered the decision. The strategy was buying at prices it could not have
known.

Corrected, the same strategy on the same data measures **1.810**. Below the bar.

The effect was worth 0.60 of Sharpe, which was the whole distance between *this
looks real* and *this does not*.

---

## 2. Why the retraction is the deliverable

A lookahead bug does not announce itself as a bug. It announces itself as a
**result** — and it arrives in exactly the form you were hoping for, at the moment
you most want to believe it. That is what makes this class of error different from
an ordinary defect: the feedback signal points the wrong way. Everything looks
better when it is wrong.

Most projects of this shape never find out. They report the 2.414.

Three instruments caught it. None of them was looking for it, and all three
existed before the result did:

### The bit-exact parity assertion

One strategy, implemented **twice** — once in C++ against the event engine, once
in Python against a pandas panel. Both compute portfolio equity in integer
micro-dollars and the two are asserted **identical**, with no tolerance parameter
anywhere in the comparison.

They diverged by **$606.07**, at session 1 of 126, on one symbol. Any tolerance at
all would have absorbed it. That divergence was a real defect in how decision-time
equity was computed, and it was the thread that unravelled the rest.

The duplication is the instrument. Two implementations agreeing is evidence *only*
while they are causally independent — delete either half, or give them a shared
core, and the assertion still passes while comparing nothing. Every printed number
survives. That property is now enforced by an import-graph guard, and its one
unenforceable gap is written down: a line-by-line transliteration of one half into
the other is textually two implementations and causally one, and no test will ever
see it. The reviewable question is *"was this derived from the specification, or
from the other implementation?"* — which a person can answer from a diff.

### The per-fill timestamp triple

Every fill records three timestamps: when the decision was made, when the price it
filled at was observed, and when the fill occurred. The assertion is
`price_ts > decision_ts`.

This turned an argument into a count: **zero honest fills in either run**. Not a
suspicion about the engine, a number.

### The empirical null

Without a measured threshold, 2.414 and 1.810 are both just numbers. The 1,512-run
sweep is what makes the correction *mean* something rather than merely be smaller.

---

## 3. The law

Somewhere in the middle of this, a second thing became the more interesting artifact.

The first instrument failure was a skipped test. One test had been silently not
running across four reported counts — 334, 343, 346, 351 — read each time as
cosmetic. It was the only test that would have revealed that a component had never
been validated. A silent failure wearing a friendly face.

That generalises, and stating it as a law is what made the later instances
recognisable in minutes rather than days:

> **Any mechanism whose output looks the same whether it is working or absent will
> eventually be absent.**

It is not a rule about tests. It is a rule about mechanisms — assertions, counters,
guards, reports. The question to ask of each is not *is it correct* but **what
would it print if someone deleted it.** If the answer is "the same thing", it is
already gone; nobody has noticed yet.

**Six instances are recorded**, five of which were live defects:

| | shape | what the output said |
|---|---|---|
| 1 | a skip read as a pass | `PASS` |
| 2 | an empty fixture directory read as a pass | `PASS` |
| 3 | an empty comparison read as a pass | `EQUIVALENT`, on zero decisions |
| 4 | a correct comparison over misrouted state | `EQUIVALENT`, on 111 decisions |
| 5 | a comparison of two processes that never started | would have said `IDENTICAL` |
| 6 | a summary counter dropped by a refactor | `128 submitted, 124 accepted, 0 rejected` |

**Instance 4 is the one that changed the response.** A live-versus-replay test
compared 111 decisions, found every one identical, and reported success. It was a
true statement about the decisions and a false impression of the system: an id
mismatch meant all 391 venue responses had been applied to nothing. The decisions
agreed because at each step the strategy's next decision did not happen to depend
on the answer it had just been given.

The first three failures were all *a report that did not say how much it measured*,
and each fix makes the report state its own denominator. Instance 4 defeated that
entirely — it reported a large, real, correctly-printed count. What caught it was
reading a different number than the verdict. Hence the extension:

> **A report that can only be checked against itself cannot be checked.**

**Instance 5 is the most useful of the six.** It happened *while the law was being
written down*, in the script verifying that a refactor was safe: 39 invocations
compared, 0 bytes of output, because both binaries had been copied away from the
libraries they linked against and neither process had started. It was caught by an
assertion written minutes earlier for no better reason than that the law was fresh
on the previous page.

That is the part that does not transfer. Noticing the pattern, naming it and
documenting it confers **no immunity**, because it is not a mistake made through
ignorance. It is the default shape of a verification: you build something to
compare two states, and the case where it compares nothing looks exactly like the
case where the two states agree.

---

## 4. What the discipline turned into

Of the six, one was found by a deliberate audit, one by an assertion written
minutes earlier, and **four by a person reading output that had already been
printed and accepted.**

> **Attention is an excellent detector and a worthless control.** It does not run on
> every commit. It does not run at 2am. It does not run once the reader trusts the
> line, which is the state every stable report eventually reaches.

So the work became converting each thing a person noticed into something the build
refuses:

- **Summaries that assert their own arithmetic.** Order counters are no longer
  trusted; they are checked against a census of the orders themselves. Six
  identities, hard failure on its own exit code, and a passing audit that states how
  many identities held — because an audit that checked nothing otherwise prints what
  an audit of everything prints.
- **A test that is supposed to fail.** "All green" is information only if red is
  reachable. One assertion is false on purpose, and the suite's pass condition
  includes the harness having evaluated a false expression and reported it.
- **Guarantees paired with checks that cannot fire.** One mechanism makes a bug
  impossible; a second one fails loudly if the first ever stops holding. The second
  is provably unreachable, which is *why* it exists — and deleting unreachable code
  is normally correct, so that pair is on a protected list with the reason attached.
- **A manifest of every test by name.** Deleting a test now fails until its name is
  removed in the same commit. My own first version covered only the C++ suite and
  would have protected everything except the single guard that mattered most — which
  is a pytest test. Its own test caught that by failing on exactly those two names.
- **Documentation checked against reality.** The README claimed "no network code, no
  API clients, no credentials" for three sessions after that stopped being true, and
  nobody noticed. A CI step now validates the load-bearing factual claims — test
  counts, ABI version, whether the guards the docs name still exist and are still
  wired in.

Each mechanism is documented with **what it does not cover**, because a guard
described more broadly than it works is worse than an admitted gap.

---

## 5. The execution seam

The other half of the platform is the part that makes a backtest checkable against
reality: **the same strategy binary, unmodified, running live and in replay, with
its decisions compared field by field.**

On 2026-09-23, against a paper venue in a regular session: 70,883 live events,
70,883 replayed, **128 decisions identical**, 128 of 128 order submissions matched
against the recording, and 434 of 434 venue responses replayed onto the right
orders. 66 of those decisions followed a partial fill — meaning the strategy was
*reacting to its own orders*, which is the case a market-data-only recording cannot
reproduce and the reason the recording format carries both directions.

A pass says the event pipeline is identical on both sides. It does **not** say a
fill-dependent strategy would agree in general, and the test is explicit about that
rather than quietly claiming it.

---

## 6. What this is not

Stated plainly, because a project like this is usually vague here.

**Not one number in this repository is a statement about markets.** Every committed
fixture is synthetic. Real market data does now reach the engine, and real orders
reach a paper broker — but nothing is fitted to them, no recording is committed, and
no table or figure rests on a single real price.

**No real strategy and no real factor.** The reference plugins are a buy-and-hold
benchmark and a moving-average crossover whose job is to be calibrated *against*. A
third is not a strategy at all: it provokes rejections and partial fills so that the
code handling those states has been run against a real venue. It loses money by
design — $2.91 of paper money, establishing that the order lifecycle works.

**One component has never been validated.** Forty-one inferred corporate-identity
splits rest on a threshold and none has been checked against a price series.
Measuring it requires the price history of companies that may have stopped trading
decades ago. It is data-dependent, not unfinished, and the repository says so rather
than reporting the threshold's output as a result.

**Paper fills are the venue's simulation**, and the market data feed used is roughly
two per cent of consolidated volume. Nothing here is a claim about execution quality.

---

## 7. Where it stands

The engineering is done in the sense that matters: **the backlog contains nothing
that does not need data.**

And the constraint is structural rather than a matter of effort. The
false-discovery threshold falls as `1/√(sample length)` and rises only as
`√(2 ln n)` in the number of variants tried. Twenty years with a full 252-variant
sweep gives a bar of 0.77; three years at a tenth of the variants gives 1.78. A
realistic factor Sharpe of 0.3–0.8 **is not detectable at all** on three years, at
any level of restraint.

That makes the data source a research-design constraint, not a convenience — and it
is why more modelling is not the next thing to do.

---

## What is actually being handed over

Not a result. A set of instruments that can tell you when you do not have one, and
a written account of the six times they were nearly not working.

The 0.60 of Sharpe is not a constant. It was one effect, on one synthetic fixture,
under one rebalance rule. What transfers is the shape of the thing: **a lookahead
does not present as a bug, it presents as a result, and the only defence is
instruments built before you need them.**

---

*Full engineering detail: [`STATUS.md`](STATUS.md) for what is proven and by which
measurement; [`AUDIT.md`](AUDIT.md) for the law and all six instances;
[`PROTECTED.md`](PROTECTED.md) for the things that look like cruft and are
load-bearing; [`FALSE_DISCOVERY.md`](FALSE_DISCOVERY.md) for the null sweep;
[`EXECUTION.md`](EXECUTION.md) for the live seam.*
