// The scorecard (JOB.md section 11): an overall grade and six sub-grades, read
// from the same years as the headline numbers. The engine scores; this draws.
// It is a separate reading from the verdict, which is one of its sub-grades.
import { SUBGRADES } from "@/lib/plain";
import { gradeWords, metric, points, type Scorecard, type Subgrade } from "@/lib/results";

/** What a sub-grade read, in words. Formatting only. */
function measuredText(sub: Subgrade): string {
  const m = sub.measured;
  const num = (k: string) => (typeof m[k] === "number" ? (m[k] as number) : null);
  switch (sub.id) {
    case "return":
      return `${points(num("cagr_pct"))} a year`;
    case "risk":
      return points(num("max_drawdown_pct"));
    case "volatility":
      return `${metric(num("volatility_pct"), 1)}%`;
    case "risk_adjusted":
      return `Sharpe ${metric(num("sharpe"))}`;
    case "versus_holding":
      return `${points(num("cagr_difference_pct"))} a year, drop ${points(num("max_drawdown_difference_pct"))}`;
    case "robustness":
      return typeof m.grade === "string" ? gradeWords(m.grade).toLowerCase() : "";
  }
}

export default function ScoreCard({ card }: { card: Scorecard }) {
  return (
    <div className="scorecard">
      <div className="score-overall">
        <span className={`score-letter grade-${card.overall.letter}`}>{card.overall.letter}</span>
        <div>
          <div className="score-title">Overall score</div>
          <div className="score-number mono">{card.overall.score.toFixed(0)} / 100</div>
          <div className="faint">A separate reading from the verdict above: a rule can score well and still be fragile.</div>
        </div>
      </div>
      <ul className="subgrades">
        {card.subgrades.map((sub) => (
          <li key={sub.id} title={SUBGRADES[sub.id]?.reads}>
            <span className={`sub-letter grade-${sub.letter ?? "none"}`}>{sub.letter ?? "–"}</span>
            <div className="sub-body">
              <div className="sub-name">
                {SUBGRADES[sub.id]?.name ?? sub.id}
                {sub.score !== null && <span className="faint mono"> {sub.score.toFixed(0)}</span>}
              </div>
              <div className="faint">{sub.status === "not_applicable" ? "Doesn't apply" : measuredText(sub)}</div>
            </div>
          </li>
        ))}
      </ul>
    </div>
  );
}
