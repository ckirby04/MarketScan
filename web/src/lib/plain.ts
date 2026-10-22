// Plain words for the Results' identifiers: gate ids, warning codes, swept
// parameter paths and objective metrics, so the page never shows a schema name.
//
// Words only. Every number shown next to these still comes from the Results,
// and the engine's own text stays available beside the plain one.

/** Each gate: a short name, and what it asks of the rule. */
export const GATES: Record<string, { name: string; asks: string }> = {
  evidence: {
    name: "Enough trades",
    asks: "There have to be enough trades, including some in the later test period, to judge the rule at all.",
  },
  oos_benchmark: {
    name: "Held up against holding",
    asks: "In the period not used to pick the setting, its return for the risk taken has to match simply holding.",
  },
  random_timing: {
    name: "Better than random timing",
    asks: "Trades held for the same lengths but on random dates have to do worse almost every time.",
  },
  multiple_testing: {
    name: "Not a lucky pick",
    asks: "After allowing for every setting that was tried, the result still has to be unlikely to be chance.",
  },
  plateau: {
    name: "Nearby settings work too",
    asks: "Settings next to the chosen one have to keep most of its edge, so it isn't a lone spike.",
  },
  survival: {
    name: "Losses you could live through",
    asks: "The worst fall, and how often reshuffled histories wipe out most of the money, have to stay within limits.",
  },
};

export function gateName(id: string): string {
  return GATES[id]?.name ?? id.replace(/_/g, " ");
}

/** Warning codes, said plainly. A code not listed falls back to the engine's message. */
const WARNINGS: Record<string, string> = {
  EXERCISE_ONLY_DATA: "The price data is test-grade, so treat this as a trial run of the method rather than a finding.",
  MOSTLY_SYNTHETIC: "Most of this history is simulated rather than real prices.",
  NO_OUT_OF_SAMPLE:
    "No later period was held back for testing, so the comparison with holding covers the same years the rule was judged on.",
  NO_SEARCH_CORRECTION:
    "Only one setting was tested, so nothing allows for it having been chosen after seeing others.",
  UNCALIBRATED_CORRECTION: "The allowance for trying many settings is an estimate that hasn't been calibrated yet.",
  FEW_TRADES_IN_SWEEP: "Some settings traded too rarely to rank. They're listed, but couldn't be chosen.",
  OPEN_POSITION_AT_END: "The rule was still holding at the end, so its last trade is valued at the final price.",
  SWEEP_EDGE: "The chosen setting is at the edge of the range tried. A wider range might choose differently.",
};

export function warningText(code: string, message: string): string {
  return WARNINGS[code] ?? message;
}

/** A swept parameter, by the last part of its path. */
const PARAMS: Record<string, { label: string; unit: "%" | "days" }> = {
  threshold_pct: { label: "Drop from the high", unit: "%" },
  drawdown_pct: { label: "Drop for this tranche", unit: "%" },
  window_days: { label: "High measured over", unit: "days" },
  pct: { label: "Exit level", unit: "%" },
  days: { label: "Holding limit", unit: "days" },
};

export function paramLabel(path: string): string {
  const key = path.slice(path.lastIndexOf("/") + 1);
  return PARAMS[key]?.label ?? key.replace(/_/g, " ");
}

/** A parameter's value with its unit: 20 -> "20%", 252 -> "252 days". */
export function paramValue(path: string, value: number): string {
  const key = path.slice(path.lastIndexOf("/") + 1);
  const unit = PARAMS[key]?.unit ?? (key.endsWith("_pct") ? "%" : key.endsWith("days") ? "days" : "");
  const shown = plainNumber(value);
  return unit === "%" ? `${shown}%` : unit ? `${shown} ${unit}` : shown;
}

/** What "best" meant when the settings were ranked. */
const OBJECTIVES: Record<string, string> = {
  cagr: "highest annual return",
  total_return: "highest total return",
  sharpe: "best return for its volatility",
  sortino: "best return for its downside swings",
  calmar: "best annual return for its worst drop",
  max_drawdown: "shallowest worst drop",
  ulcer_index: "smallest, shortest slumps",
};

export function objectiveLabel(metric: string): string {
  return OBJECTIVES[metric] ?? metric.replace(/_/g, " ");
}

/** A number without trailing zeros: 20 -> "20", 2.5 -> "2.5". Formatting only. */
export function plainNumber(v: number): string {
  return String(Number(v.toFixed(4)));
}

/** The scorecard's sub-grades (JOB.md section 11): a name, and what each reads. */
export const SUBGRADES: Record<string, { name: string; reads: string }> = {
  return: { name: "Return", reads: "Annual return" },
  risk: { name: "Risk", reads: "Worst drop" },
  volatility: { name: "Volatility", reads: "How much it swings, a year" },
  risk_adjusted: { name: "Risk-adjusted", reads: "Return for its swings (Sharpe)" },
  versus_holding: { name: "Versus holding", reads: "Return and worst drop against holding" },
  robustness: { name: "Robustness", reads: "The verdict" },
};
