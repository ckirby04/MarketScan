"""The research job's grading, from the research layer's side.

Specification: contracts/research/JOB.md. The engine runs the strategy and
exports a trace -- each run's equity curve and fills, and the bars -- and this
module recomputes everything after the runs from that trace and the
specification alone: trades, windows, metrics, selection, the random-timing
null, the effective-N correction, the plateau, the outcome range, the gates and
the grade. It was written from JOB.md, not from src/marketscan/research/job.cpp;
see docs/AUDIT.md "Deliberate duplication".

The two must agree. research/tests/test_grading.py holds them to it on a
committed trace.

ONE THING THIS DOES NOT RECOMPUTE

The bootstrap paths are regenerated here from the generator, and the first is
checked close for close against the engine's, but the strategy is not re-run on
them: the trace carries each path's first and last equity and the strategy's
max drawdown, and the outcome range, the share of paths beating holding and the
survival gate's path share are computed from those. Re-running the rule on a
thousand paths would need this layer to mark positions the way the engine's
ledger does, which is not part of any specification. The trades' descriptive
fields (entry price, holding days, worst excursion) are likewise not
recomputed; JOB.md section 4 says so.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
from dataclasses import dataclass, field

import numpy as np

NAN = float("nan")
MASK64 = (1 << 64) - 1


# --- Section 6's generator --------------------------------------------------------


class SplitMix64:
    def __init__(self, seed: int):
        self.state = seed & MASK64

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        return z ^ (z >> 31)

    def draw(self, n: int) -> int:
        return self.next() % n


def canonical_form(form: dict) -> str:
    body = {k: v for k, v in form.items() if k not in ("prompt", "provenance")}
    return json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def seed_of(canonical: str) -> int:
    return int.from_bytes(hashlib.sha256(canonical.encode("utf-8")).digest()[:8], "little")


def percentile(values, q: float) -> float:
    xs = sorted(v for v in values if not math.isnan(v))
    if not xs:
        return NAN
    h = (len(xs) - 1) * q
    lo = math.floor(h)
    if lo == len(xs) - 1:
        return xs[lo]
    return xs[lo] + (h - lo) * (xs[lo + 1] - xs[lo])


def better(metric: str, a: float, b: float) -> bool:
    """Section 5. A null is worse than any number."""
    if math.isnan(a):
        return False
    if math.isnan(b):
        return True
    return a < b if metric == "ulcer_index" else a > b


# --- Section 4: one run -----------------------------------------------------------


@dataclass
class Trade:
    first_buy: int
    sell: int | None = None
    cost: float = 0.0
    quantity: float = 0.0
    bought: float = 0.0
    value: float = 0.0
    exit_price: float = 0.0
    reason: int = 0

    def return_pct(self) -> float:
        return 100.0 * (self.value / self.cost - 1.0)


@dataclass
class Run:
    equity: list[float]
    trades: list[Trade]
    exposure: list[bool]


def analyse(equity, fills, closes, e) -> Run:
    trades, open_ = [], None
    for f in fills:
        if f["side"] == "buy":
            if open_ is None:
                open_ = Trade(first_buy=f["bar"])
            open_.cost += f["price"] * f["quantity"] + f["commission"]
            open_.quantity += f["quantity"]
            open_.bought += f["price"] * f["quantity"]
        elif open_ is not None:
            open_.sell = f["bar"]
            open_.value = f["price"] * f["quantity"] - f["commission"]
            open_.exit_price = f["price"]
            open_.reason = f["tag"] & 0xFF
            trades.append(open_)
            open_ = None
    if open_ is not None:
        open_.value = closes[e] * open_.quantity
        open_.exit_price = closes[e]
        trades.append(open_)
    exposure = [False] * len(closes)
    for t in trades:
        last = t.sell if t.sell is not None else e
        for b in range(t.first_buy + 1, last + 1):
            exposure[b] = True
    return Run(list(equity), trades, exposure)


METRIC_KEYS = (
    "total_return_pct", "cagr_pct", "volatility_pct", "sharpe", "sortino", "max_drawdown_pct",
    "max_drawdown_days", "calmar", "ulcer_index", "time_in_market_pct", "n_entries",
    "win_rate_pct", "avg_trade_return_pct", "worst_trade_pct",
)

OBJECTIVE = {
    "cagr": "cagr_pct", "sharpe": "sharpe", "sortino": "sortino", "calmar": "calmar",
    "max_drawdown": "max_drawdown_pct", "ulcer_index": "ulcer_index",
    "total_return": "total_return_pct",
}


def metrics(levels_all, days, exposure, trades, s, a, b) -> dict:
    out = {k: NAN for k in METRIC_KEYS}
    out["n_entries"] = sum(1 for t in trades if a <= t.first_buy <= b)
    first = a - 1 if a > s else a
    if b <= first:
        return out
    L = levels_all[first : b + 1]
    D = days[first : b + 1]
    m = len(L) - 1

    out["total_return_pct"] = 100.0 * (L[m] / L[0] - 1.0)
    span = D[m] - D[0]
    if span > 0:
        out["cagr_pct"] = 100.0 * (math.pow(L[m] / L[0], 365.25 / span) - 1.0)

    r = [L[i] / L[i - 1] - 1.0 for i in range(1, m + 1)]
    total = 0.0
    for x in r:
        total += x
    mean = total / m
    if m >= 2:
        sq = 0.0
        for x in r:
            sq += (x - mean) * (x - mean)
        sd = math.sqrt(sq / (m - 1))
        out["volatility_pct"] = 100.0 * sd * math.sqrt(252.0)
        if sd != 0.0:
            out["sharpe"] = mean / sd * math.sqrt(252.0)
    down = 0.0
    for x in r:
        d = min(x, 0.0)
        down += d * d
    dd = math.sqrt(down / m)
    if dd != 0.0:
        out["sortino"] = mean / dd * math.sqrt(252.0)

    peak, peak_day, under, longest = L[0], D[0], False, 0
    worst, ulcer = 0.0, 0.0
    for x, day in zip(L, D):
        if x >= peak:
            if under:
                longest = max(longest, day - peak_day)
                under = False
            peak, peak_day = x, day
        else:
            under = True
        draw = 100.0 * (x / peak - 1.0)
        worst = min(worst, draw)
        ulcer += draw * draw
    if under:
        longest = max(longest, D[m] - peak_day)
    out["max_drawdown_pct"] = worst
    out["max_drawdown_days"] = float(longest)
    if worst != 0.0 and not math.isnan(out["cagr_pct"]):
        out["calmar"] = out["cagr_pct"] / abs(worst)
    out["ulcer_index"] = math.sqrt(ulcer / (m + 1))
    exposed = sum(1 for t in range(first + 1, b + 1) if exposure[t])
    out["time_in_market_pct"] = 100.0 * exposed / m

    closed = [t for t in trades if t.sell is not None and a <= t.sell <= b]
    if closed:
        rets = [t.return_pct() for t in closed]
        out["win_rate_pct"] = 100.0 * sum(1.0 for x in rets if x > 0.0) / len(rets)
        total = 0.0
        for x in rets:
            total += x
        out["avg_trade_return_pct"] = total / len(rets)
        out["worst_trade_pct"] = min(rets)
    return out


@dataclass
class Series:
    days: list[int]
    closes: list[float]
    s: int
    o: int
    e: int
    dates: list[str] = field(default_factory=list)


def series_of(trace_series: dict) -> Series:
    dates = trace_series["dates"]
    epoch = dt.date(1970, 1, 1)
    days = [(dt.date.fromisoformat(d) - epoch).days for d in dates]
    return Series(days, trace_series["closes"], trace_series["s"], trace_series["o"],
                  trace_series["e"], dates)


def windows(series: Series, split: bool) -> dict:
    w = {"full": (series.s, series.e)}
    if split:
        w["in_sample"] = (series.s, series.o - 1)
        w["out_of_sample"] = (series.o, series.e)
    return w


def measure(run: Run, series: Series, split: bool) -> dict:
    return {
        name: metrics(run.equity, series.days, run.exposure, run.trades, series.s, a, b)
        for name, (a, b) in windows(series, split).items()
    }


# --- Sections 6 to 9 --------------------------------------------------------------


def timing_metric(series: Series, exposure, metric: str) -> float:
    v = [1.0] * len(series.closes)
    for t in range(series.s + 1, series.e + 1):
        c = series.closes
        v[t] = v[t - 1] * (1.0 + (c[t] / c[t - 1] - 1.0)) if exposure[t] else v[t - 1]
    return metrics(v, series.days, exposure, [], series.s, series.s, series.e)[OBJECTIVE[metric]]


def random_timing(series: Series, run: Run, metric: str, draws: int, rng: SplitMix64) -> dict:
    strategy = timing_metric(series, run.exposure, metric)
    lengths = [(t.sell if t.sell is not None else series.e) - t.first_buy for t in run.trades]
    if not lengths:
        return {"values": [], "percentile": 0.0, "p": 1.0}
    bars, held = series.e - series.s, sum(lengths)
    values = []
    for _ in range(draws):
        order = list(lengths)
        for i in range(len(order) - 1, 0, -1):
            j = rng.draw(i + 1)
            order[i], order[j] = order[j], order[i]
        gaps = sorted(rng.draw(bars - held + 1) for _ in order)
        exposure = [False] * len(series.closes)
        before = 0
        for g, length in zip(gaps, order):
            start = series.s + g + before
            for t in range(start + 1, start + length + 1):
                exposure[t] = True
            before += length
        values.append(timing_metric(series, exposure, metric))
    below = equal = above = 0
    for v in values:
        if better(metric, strategy, v):
            below += 1
        elif better(metric, v, strategy):
            above += 1
        else:
            equal += 1
    n = len(values)
    return {
        "values": values,
        "percentile": 100.0 * (below + 0.5 * equal) / n,
        "p": (1 + above + equal) / (1 + n),
    }


def effective_n(runs: list[Run], series: Series) -> float:
    rows = []
    for run in runs:
        q = run.equity
        r = np.array([q[t] / q[t - 1] - 1.0 for t in range(series.s + 1, series.e + 1)])
        if np.var(r) > 0.0:
            rows.append(r)
    if len(rows) <= 1:
        return 1.0
    eig = np.sort(np.linalg.eigvalsh(np.corrcoef(np.array(rows))))[::-1]
    running, total = 0.0, float(np.sum(eig))
    for k, v in enumerate(eig):
        running += float(v)
        if running >= 0.95 * total:
            return float(k + 1)
    return float(len(eig))


def edge(metric: str, point: float, bench: float) -> float:
    if math.isnan(point) or math.isnan(bench):
        return NAN
    return bench - point if metric == "ulcer_index" else point - bench


def plateau(metric, grid, objective, selected, bench) -> float:
    own = edge(metric, objective[selected], bench)
    if math.isnan(own) or own <= 0.0:
        return NAN
    edges = []
    for i, point in enumerate(grid):
        diffs = [a - b for a, b in zip(point, grid[selected]) if a != b]
        if len(diffs) == 1 and diffs[0] in (1, -1):
            e = edge(metric, objective[i], bench)
            if math.isnan(e):
                return NAN
            edges.append(e)
    if not edges:
        return NAN
    edges.sort()
    n = len(edges)
    median = edges[n // 2] if n % 2 else (edges[n // 2 - 1] + edges[n // 2]) / 2.0
    return median / own


def resample_closes(series: Series, rng: SplitMix64) -> list[float]:
    c = series.closes
    R = [c[t] / c[t - 1] - 1.0 for t in range(series.s + 1, series.e + 1)]
    out = list(c)
    if not R:
        return out
    j = rng.draw(len(R))
    close = c[series.s]
    for t in range(series.s + 1, series.e + 1):
        if t > series.s + 1:
            j = rng.draw(len(R)) if rng.draw(63) == 0 else (j + 1) % len(R)
        close = close * (1.0 + R[j])
        out[t] = close
    return out


def cagr(first: float, last: float, days: int) -> float:
    return NAN if days <= 0 else 100.0 * (math.pow(last / first, 365.25 / days) - 1.0)


# --- Everything -----------------------------------------------------------------


def grade(trace: dict) -> dict:
    """Regrades a job trace. Returns the Results' graded parts, in their shape."""
    metric, split = trace["metric"], trace["has_split"]
    key = OBJECTIVE[metric]
    asset = series_of(trace["asset"])
    canonical = canonical_form(trace["form"])
    rng = SplitMix64(seed_of(canonical))

    runs = [analyse(p["equity"], p["fills"], asset.closes, asset.e) for p in trace["points"]]
    measured = [measure(r, asset, split) for r in runs]
    bench_series = [series_of(b) for b in trace["benchmarks"]]
    bench_runs = [analyse(b["equity"], b["fills"], s.closes, s.e)
                  for b, s in zip(trace["benchmarks"], bench_series)]
    bench_measured = [measure(r, s, split) for r, s in zip(bench_runs, bench_series)]
    primary = bench_measured[0]

    ranking = "in_sample" if split else "full"
    judged = "out_of_sample" if split else "full"

    # Section 2's grid, last dimension fastest.
    grid = [[]]
    for dim in trace["dims"]:
        grid = [g + [v] for g in grid for v in range(len(dim["values"]))]

    # Section 5.
    objective = [m[ranking][key] for m in measured]
    eligible = [m[ranking]["n_entries"] >= trace["min_trades"] for m in measured]
    pool = [i for i in range(len(runs)) if eligible[i]] or list(range(len(runs)))
    selected = pool[0]
    for i in pool[1:]:
        if better(metric, objective[i], objective[selected]):
            selected = i
    best, best_run = measured[selected], runs[selected]

    # Sections 6 to 8.
    null = random_timing(asset, best_run, metric, trace["null_draws"], rng)
    swept = bool(trace["dims"])
    n_eff = effective_n(runs, asset) if swept else NAN
    p_corrected = 1.0 - math.pow(1.0 - null["p"], n_eff) if swept else NAN
    ratio = plateau(metric, grid, objective, selected, primary[ranking][key]) if swept else NAN

    # Section 9: regenerate the paths to check the generator, and grade the runs.
    paths = trace["bootstrap"]["paths"]
    first_path = None
    for n in range(len(paths)):
        closes = resample_closes(asset, rng)
        if n == 0:
            first_path = closes
    span = asset.days[asset.e] - asset.days[asset.s]
    s_cagr = [cagr(p["strategy"][0], p["strategy"][1], span) for p in paths]
    b_cagr = [cagr(p["benchmark"][0], p["benchmark"][1], span) for p in paths]
    ruined = [(p["strategy_max_drawdown_pct"] if p["strategy_max_drawdown_pct"] is not None
               else NAN) < -90.0 for p in paths]
    count = max(len(paths), 1)

    # Section 10.
    gates = []
    entries = best["full"]["n_entries"]
    passed = entries >= 5
    if split:
        passed = passed and best["out_of_sample"]["n_entries"] >= 2
    gates.append(("evidence", "pass" if passed else "fail"))
    gates.append(("oos_benchmark",
                  "pass" if better(metric, best[judged][key], primary[judged][key]) else "fail"))
    gates.append(("random_timing", "pass" if null["percentile"] >= 95.0 else "fail"))
    if swept:
        gates.append(("multiple_testing", "pass" if p_corrected < 0.05 else "fail"))
        gates.append(("plateau", "pass" if not math.isnan(ratio) and ratio >= 0.5 else "fail"))
    else:
        gates += [("multiple_testing", "not_applicable"), ("plateau", "not_applicable")]
    share = sum(ruined) / count
    worst = best["full"]["max_drawdown_pct"]
    gates.append(("survival", "pass" if worst > -90.0 and share < 0.05 else "fail"))

    fails_to = {"evidence": "insufficient_evidence", "oos_benchmark": "not_supported"}
    verdict, deciding = "supported", None
    for gid, status in gates:
        if status == "fail":
            verdict, deciding = fails_to.get(gid, "fragile"), gid
            break

    def window_block(m, s: Series):
        return {name: {"from": s.dates[a], "to": s.dates[b],
                       "metrics": {k: m[name][k] for k in METRIC_KEYS}}
                for name, (a, b) in windows(s, split).items()}

    return {
        "form_sha256": hashlib.sha256(canonical.encode("utf-8")).hexdigest(),
        "grade": verdict,
        "deciding_gate": deciding,
        "gates": gates,
        "selected_index": selected,
        "selected_windows": window_block(best, asset),
        "benchmark_windows": [window_block(m, s) for m, s in zip(bench_measured, bench_series)],
        "objective": objective,
        "eligible": eligible,
        "effective_n": n_eff,
        "plateau_ratio": ratio,
        "random_timing": {
            "n_simulations": len(null["values"]),
            "null_p5": percentile(null["values"], 0.05) if null["values"] else NAN,
            "null_p50": percentile(null["values"], 0.50) if null["values"] else NAN,
            "null_p95": percentile(null["values"], 0.95) if null["values"] else NAN,
            "strategy_percentile": null["percentile"],
            "p_value": null["p"],
            "p_value_corrected": p_corrected,
        },
        "first_path_closes": first_path,
        "bootstrap": {
            "strategy": [percentile(s_cagr, q) for q in (0.05, 0.50, 0.95)],
            "benchmark": [percentile(b_cagr, q) for q in (0.05, 0.50, 0.95)],
            "prob_beats_benchmark": sum(1 for a, b in zip(s_cagr, b_cagr) if better("cagr", a, b))
            / count,
            "share_paths_below_minus_90_pct": share,
        },
        "trades": [
            {"entry_date": asset.dates[t.first_buy],
             "exit_date": asset.dates[t.sell] if t.sell is not None else None,
             "return_pct": t.return_pct()}
            for t in best_run.trades
        ],
    }
