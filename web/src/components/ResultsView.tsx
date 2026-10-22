"use client";

// A Results document, in the order SPEC_RESEARCH_VIEW.md fixes: the verdict,
// the gates behind it, the stats, and only then analysis -- because analysis,
// however careful, can mislead, so it comes last and answers to everything
// above it.
//
// Every number is a field of the document; this formats, it never computes.
import { useState } from "react";

import EquityChart from "@/components/EquityChart";
import {
  gradeWords,
  metric,
  points,
  share,
  type Curve,
  type Metrics,
  type Results,
  type Windows,
} from "@/lib/results";

const GRADE_CLASS: Record<string, string> = {
  supported: "good",
  fragile: "warning",
  not_supported: "serious",
  insufficient_evidence: "dim",
};

function WindowsTable({ results }: { results: Results }) {
  const columns: [string, Windows][] = [["rule", results.stats.selected.windows]];
  const bench = results.stats.benchmarks[0];
  if (bench) columns.push([bench.symbol, bench.windows]);
  const names = ["in_sample", "out_of_sample", "full"] as const;
  const cells: { title: string; m: Metrics; from: string; to: string }[] = [];
  for (const [who, windows] of columns) {
    for (const name of names) {
      const w = windows[name];
      if (w) cells.push({ title: `${who} ${name.replace(/_/g, " ")}`, m: w.metrics, from: w.from, to: w.to });
    }
  }
  const rows: [string, (m: Metrics) => string][] = [
    ["total return", (m) => points(m.total_return_pct)],
    ["CAGR", (m) => points(m.cagr_pct)],
    ["volatility", (m) => `${metric(m.volatility_pct)}%`],
    ["Sharpe", (m) => metric(m.sharpe)],
    ["Sortino", (m) => metric(m.sortino)],
    ["max drawdown", (m) => points(m.max_drawdown_pct)],
    ["longest drawdown, days", (m) => metric(m.max_drawdown_days, 0)],
    ["Calmar", (m) => metric(m.calmar)],
    ["ulcer index", (m) => metric(m.ulcer_index)],
    ["time in market", (m) => `${metric(m.time_in_market_pct, 1)}%`],
    ["entries", (m) => String(m.n_entries)],
    ["win rate", (m) => `${metric(m.win_rate_pct, 1)}%`],
    ["average trade", (m) => points(m.avg_trade_return_pct)],
    ["worst trade", (m) => points(m.worst_trade_pct)],
  ];
  return (
    <div className="scroll">
      <table>
        <thead>
          <tr>
            <th>metric</th>
            {cells.map((c) => (
              <th key={c.title}>{c.title}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map(([label, get]) => (
            <tr key={label}>
              <td className="dim">{label}</td>
              {cells.map((c) => (
                <td key={c.title} className="mono">
                  {get(c.m)}
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
      {cells.map((c) => (
        <div key={c.title} className="faint">
          {c.title}: {c.from} to {c.to}
        </div>
      ))}
    </div>
  );
}

function Sweep({ results }: { results: Results }) {
  const s = results.stats.parameter_surface;
  if (!s) return <p className="dim">Nothing was swept, or the surface was not asked for.</p>;
  return (
    <div className="scroll">
      <p className="dim">
        objective {s.metric}; selected point {s.selected_index + 1} of {s.points.length}; effective
        variants {metric(s.effective_n, 0)}; plateau ratio {metric(s.plateau_ratio)}
      </p>
      <table>
        <thead>
          <tr>
            {s.swept.map((p) => (
              <th key={p}>{p.slice(p.lastIndexOf("/") + 1).replace(/_/g, " ")}</th>
            ))}
            <th>in sample</th>
            <th>out of sample</th>
            <th>entries</th>
            <th></th>
          </tr>
        </thead>
        <tbody>
          {s.points.map((p, i) => (
            <tr key={i}>
              {s.swept.map((path) => (
                <td key={path} className="mono">
                  {metric(p.params[path], 4)}
                </td>
              ))}
              <td className="mono">{metric(p.in_sample_metric)}</td>
              <td className="mono">{metric(p.out_of_sample_metric)}</td>
              <td className="mono">{p.n_entries}</td>
              <td className={i === s.selected_index ? "good" : "faint"}>
                {i === s.selected_index ? "selected" : p.excluded ? "too few entries" : ""}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function Timing({ results }: { results: Results }) {
  const t = results.stats.random_timing;
  const rows: [string, string][] = [
    ["random alternatives", String(t.n_simulations)],
    [`null ${t.metric}, 5th percentile`, metric(t.null_p5)],
    [`null ${t.metric}, median`, metric(t.null_p50)],
    [`null ${t.metric}, 95th percentile`, metric(t.null_p95)],
    ["the rule's percentile", metric(t.strategy_percentile, 1)],
    ["p-value", metric(t.p_value, 4)],
    ["p-value, corrected for the search", metric(t.p_value_corrected, 4)],
  ];
  return (
    <>
      <table>
        <tbody>
          {rows.map(([label, value]) => (
            <tr key={label}>
              <td className="dim">{label}</td>
              <td className="mono">{value}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <p className="faint">
        Each alternative keeps the rule&apos;s holding periods and places them at random dates. Both sides are
        measured frictionless, so the comparison is about timing alone.
      </p>
    </>
  );
}

function Range({ results }: { results: Results }) {
  const b = results.stats.bootstrap;
  const row = (label: string, r: { cagr_p5_pct: number; cagr_p50_pct: number; cagr_p95_pct: number }) => (
    <tr>
      <td className="dim">{label}</td>
      <td className="mono">{points(r.cagr_p5_pct)}</td>
      <td className="mono">{points(r.cagr_p50_pct)}</td>
      <td className="mono">{points(r.cagr_p95_pct)}</td>
    </tr>
  );
  return (
    <>
      <table>
        <thead>
          <tr>
            <th>CAGR across resampled histories</th>
            <th>5th</th>
            <th>median</th>
            <th>95th</th>
          </tr>
        </thead>
        <tbody>
          {row("the rule", b.strategy)}
          {row("holding it", b.benchmark)}
        </tbody>
      </table>
      <p className="dim">
        The rule beat holding on {share(b.prob_beats_benchmark)} of paths; {share(b.share_paths_below_minus_90_pct)}{" "}
        of paths fell below -90%.
      </p>
      <p className="faint">
        {b.n_paths} paths, blocks of about {b.mean_block_days} days. This is the spread of results across
        reshuffled histories -- how much the result depends on the order things happened in. It is not a forecast
        and not an expected return.
      </p>
    </>
  );
}

function Trades({ results }: { results: Results }) {
  return (
    <div className="scroll">
      <table>
        <thead>
          <tr>
            <th>entry</th>
            <th>entry price</th>
            <th>exit</th>
            <th>exit price</th>
            <th>return</th>
            <th>days</th>
            <th>worst</th>
            <th>why</th>
          </tr>
        </thead>
        <tbody>
          {results.stats.trades.map((t, i) => (
            <tr key={i}>
              <td className="mono">{t.entry_date}</td>
              <td className="mono">{t.entry_price.toFixed(2)}</td>
              <td className="mono">{t.exit_date ?? "open"}</td>
              <td className="mono">{t.exit_price.toFixed(2)}</td>
              <td className={`mono ${t.return_pct > 0 ? "good" : t.return_pct < 0 ? "serious" : ""}`}>
                {points(t.return_pct)}
              </td>
              <td className="mono">{t.holding_days}</td>
              <td className="mono">{points(t.max_adverse_pct)}</td>
              <td className="dim">
                {t.exit_reason}
                {t.window === "out_of_sample" ? ", OOS" : ""}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function Data({ results }: { results: Results }) {
  return (
    <>
      {results.stats.data.map((d) => (
        <div key={d.series_id} className="mono">
          {d.series_id} {d.used_from} to {d.used_to} {d.quality_tier}
          {d.synthetic ? " partly synthetic" : ""}
        </div>
      ))}
      {results.stats.regimes.length === 0 && (
        <p className="faint">
          No regime breakdown: named stress periods come from the data catalog, which is not built yet.
        </p>
      )}
      <p className="faint">
        run {results.run.run_id}, {results.run.started_at}, {results.run.duration_ms} ms
      </p>
    </>
  );
}

const TABS = ["Equity", "Windows", "Sweep", "Timing", "Range", "Trades", "Data"] as const;

export default function ResultsView({ results, curves }: { results: Results; curves: Curve[] }) {
  const [tab, setTab] = useState<(typeof TABS)[number]>("Equity");
  const v = results.verdict;
  const h = v.headline;
  return (
    <>
      <div className="heading">HISTORICAL EVIDENCE</div>
      <div>
        <span className={`grade ${GRADE_CLASS[v.grade] ?? ""}`}>{gradeWords(v.grade)}</span>
        {v.data_grade !== "research" && <span className="warning"> &nbsp;! unverified data</span>}
      </div>
      <div>
        out of sample <span className="mono">{points(h.strategy_oos_cagr_pct)}</span> CAGR, against{" "}
        <span className="mono">{points(h.benchmark_oos_cagr_pct)}</span> for the benchmark
      </div>
      <div>
        max drawdown <span className="mono">{points(h.strategy_oos_max_drawdown_pct)}</span>, against{" "}
        <span className="mono">{points(h.benchmark_oos_max_drawdown_pct)}</span>
      </div>
      <div className="dim">
        resampled histories: 5th <span className="mono">{points(h.outcome_range.cagr_p5_pct)}</span>, median{" "}
        <span className="mono">{points(h.outcome_range.cagr_p50_pct)}</span>, 95th{" "}
        <span className="mono">{points(h.outcome_range.cagr_p95_pct)}</span> (a spread, not a forecast)
      </div>
      {results.warnings.map((w) => (
        <div key={w.code} className="warning">
          ! {w.code} {w.message}
        </div>
      ))}

      <div className="heading">WHY</div>
      <table>
        <tbody>
          {v.gates.map((g) => (
            <tr key={g.id}>
              <td
                className={`mono ${g.status === "pass" ? "good" : g.status === "fail" ? "serious" : "faint"}`}
                style={{ width: 60 }}
              >
                {g.status === "pass" ? "PASS" : g.status === "fail" ? "FAIL" : "--"}
              </td>
              <td className={g.id === v.deciding_gate ? "" : "dim"} style={{ textAlign: "left", width: 150 }}>
                {g.id}
              </td>
              <td style={{ textAlign: "left", whiteSpace: "normal" }}>{g.summary}</td>
            </tr>
          ))}
        </tbody>
      </table>

      <div className="heading">STATS</div>
      <div className="tabs" role="tablist">
        {TABS.map((name) => (
          <button key={name} role="tab" aria-selected={tab === name} onClick={() => setTab(name)}>
            {name}
          </button>
        ))}
      </div>
      {tab === "Equity" && <EquityChart curves={curves} />}
      {tab === "Windows" && <WindowsTable results={results} />}
      {tab === "Sweep" && <Sweep results={results} />}
      {tab === "Timing" && <Timing results={results} />}
      {tab === "Range" && <Range results={results} />}
      {tab === "Trades" && <Trades results={results} />}
      {tab === "Data" && <Data results={results} />}

      <div className="heading">ANALYSIS - written by AI from the numbers above</div>
      <p className="faint">
        No analysis: the assistant is not connected yet (part R2). Everything above is the engine&apos;s, and
        stands without it.
      </p>
    </>
  );
}
