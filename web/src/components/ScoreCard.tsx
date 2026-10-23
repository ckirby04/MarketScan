// The scorecard (JOB.md section 11): an overall grade and six sub-grades, read
// from the same years as the headline numbers. The engine scores; this draws.
// It is a separate reading from the verdict, which is one of its sub-grades.
import { useId } from "react";

import { SUBGRADES } from "@/lib/plain";
import { gradeWords, metric, points, type Curve, type Scorecard, type Subgrade } from "@/lib/results";

/** What a sub-grade read, in words. Formatting only. */
function measuredText(sub: Subgrade): string {
  const m = sub.measured;
  const num = (k: string) => (typeof m[k] === "number" ? (m[k] as number) : null);
  switch (sub.id) {
    case "return":
      return `${points(num("cagr_pct"))} a year`;
    case "risk":
      return `${points(num("max_drawdown_pct"))} worst drop`;
    case "volatility":
      return `${metric(num("volatility_pct"), 1)}% a year`;
    case "risk_adjusted":
      return `Sharpe ${metric(num("sharpe"))}`;
    case "versus_holding":
      return `${points(num("cagr_difference_pct"))} a year, drop ${points(num("max_drawdown_difference_pct"))}`;
    case "robustness":
      return typeof m.grade === "string" ? gradeWords(m.grade).toLowerCase() : "";
  }
}

const TONE: Record<string, string> = {
  A: "var(--status-good)",
  B: "var(--status-good)",
  C: "var(--status-warning)",
  D: "var(--status-serious)",
  F: "var(--status-serious)",
};

/** The rule's growth of 1, small and unlabelled; the Details tab has the real chart. */
function Spark({ curves }: { curves: Curve[] }) {
  // One page can show several runs; each gradient needs its own id.
  const fill = `spark-${useId().replace(/:/g, "")}`;
  const c = curves.find((curve) => !curve.is_benchmark && curve.times.length > 1);
  if (!c) return null;
  const W = 320;
  const H = 44;
  const values = c.normalised.map((v) => Math.log(Math.max(v, 1e-4)));
  const lo = Math.min(...values);
  const hi = Math.max(...values);
  const step = Math.max(1, Math.floor(values.length / 160));
  let line = "";
  for (let i = 0; i < values.length; i += step) {
    const x = (i / (values.length - 1)) * W;
    const y = 3 + (1 - (values[i] - lo) / (hi - lo || 1)) * (H - 6);
    line += `${line ? "L" : "M"}${x.toFixed(1)},${y.toFixed(1)}`;
  }
  return (
    <svg className="spark" viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" aria-hidden="true">
      <defs>
        <linearGradient id={fill} x1="0" x2="0" y1="0" y2="1">
          <stop offset="0" stopColor="var(--series-1)" stopOpacity="0.25" />
          <stop offset="1" stopColor="var(--series-1)" stopOpacity="0" />
        </linearGradient>
      </defs>
      <path d={`${line}L${W},${H}L0,${H}Z`} fill={`url(#${fill})`} />
      <path d={line} fill="none" stroke="var(--series-1)" strokeWidth="1.8" vectorEffect="non-scaling-stroke" />
    </svg>
  );
}

export default function ScoreCard({ card, curves }: { card: Scorecard; curves: Curve[] }) {
  return (
    <div className="scorecard">
      <div className="score-hero">
        <span className={`score-letter grade-${card.overall.letter}`}>{card.overall.letter}</span>
        <div>
          <div className="score-title">Overall score</div>
          <div className="score-number">
            {card.overall.score.toFixed(0)}
            <small>/ 100</small>
          </div>
          <div className="score-meter" aria-hidden="true">
            <div style={{ width: `${card.overall.score}%` }} />
          </div>
          <div className="faint">A separate reading from the verdict: a rule can score well and still be fragile.</div>
          <Spark curves={curves} />
        </div>
      </div>
      <ul className="subgrades">
        {card.subgrades.map((sub) => (
          <li key={sub.id} title={SUBGRADES[sub.id]?.reads}>
            <span className={`sub-letter grade-${sub.letter ?? "none"}`}>{sub.letter ?? "–"}</span>
            <div className="sub-body">
              <div className="sub-name">
                {SUBGRADES[sub.id]?.name ?? sub.id}
                {sub.score !== null && <span className="faint">{sub.score.toFixed(0)}</span>}
              </div>
              {sub.score !== null && (
                <div className="sub-bar" aria-hidden="true">
                  <div style={{ width: `${sub.score}%`, background: TONE[sub.letter ?? ""] ?? "var(--text-faint)" }} />
                </div>
              )}
              <div className="faint">{sub.status === "not_applicable" ? "Doesn't apply" : measuredText(sub)}</div>
            </div>
          </li>
        ))}
      </ul>
    </div>
  );
}
