// The Results document's shape, and how its numbers are written.
//
// The page computes nothing: every figure on it is a field of the Results the
// engine produced (contracts/research/results.schema.json), and these helpers
// only format one. They match the desktop app's panels/format.cpp, so a number
// reads the same in both.

export type Num = number | null;

export interface Metrics {
  total_return_pct: Num;
  cagr_pct: Num;
  volatility_pct: Num;
  sharpe: Num;
  sortino: Num;
  max_drawdown_pct: Num;
  max_drawdown_days: Num;
  calmar: Num;
  ulcer_index: Num;
  time_in_market_pct: Num;
  n_entries: number;
  win_rate_pct: Num;
  avg_trade_return_pct: Num;
  worst_trade_pct: Num;
}

export interface Window {
  from: string;
  to: string;
  metrics: Metrics;
}

export type Windows = { full: Window; in_sample?: Window; out_of_sample?: Window };

export interface Gate {
  id: string;
  status: "pass" | "fail" | "not_applicable";
  summary: string;
}

export interface Trade {
  entry_date: string;
  entry_price: number;
  exit_date: string | null;
  exit_price: number;
  return_pct: number;
  holding_days: number;
  max_adverse_pct: number;
  exit_reason: string;
  window: string;
}

export interface SurfacePoint {
  params: Record<string, number>;
  in_sample_metric: Num;
  out_of_sample_metric?: Num;
  n_entries: number;
  excluded: boolean;
}

export interface Results {
  run: { run_id: string; started_at: string; duration_ms: number };
  verdict: {
    grade: "supported" | "fragile" | "not_supported" | "insufficient_evidence";
    data_grade: "research" | "exercise_only";
    deciding_gate: string | null;
    headline: {
      strategy_oos_cagr_pct: Num;
      benchmark_oos_cagr_pct: Num;
      strategy_oos_max_drawdown_pct: Num;
      benchmark_oos_max_drawdown_pct: Num;
      outcome_range: { cagr_p5_pct: number; cagr_p50_pct: number; cagr_p95_pct: number };
    };
    gates: Gate[];
  };
  stats: {
    selected: { params: Record<string, number>; windows: Windows };
    benchmarks: { symbol: string; security_id: string; windows: Windows }[];
    trades: Trade[];
    parameter_surface: null | {
      swept: string[];
      metric: string;
      points: SurfacePoint[];
      selected_index: number;
      plateau_ratio: Num;
      effective_n: number;
    };
    regimes: unknown[];
    random_timing: {
      n_simulations: number;
      metric: string;
      null_p5: Num;
      null_p50: Num;
      null_p95: Num;
      strategy_percentile: number;
      p_value: number;
      p_value_corrected: Num;
    };
    bootstrap: {
      n_paths: number;
      mean_block_days: number;
      strategy: { cagr_p5_pct: number; cagr_p50_pct: number; cagr_p95_pct: number };
      benchmark: { cagr_p5_pct: number; cagr_p50_pct: number; cagr_p95_pct: number };
      prob_beats_benchmark: number;
      share_paths_below_minus_90_pct: number;
    };
    data: {
      security_id: string;
      series_id: string;
      quality_tier: string;
      used_from: string;
      used_to: string;
      synthetic: unknown;
    }[];
  };
  warnings: { code: string; message: string }[];
}

export interface Curve {
  id: string;
  label: string;
  is_benchmark: boolean;
  times: number[];
  equity: number[];
  normalised: number[];
  drawdown: number[];
}

export interface JobStatus {
  job_id: string;
  state: "queued" | "running" | "done" | "refused" | "failed";
  stage: string;
  done: number;
  total: number;
  grade: string | null;
  errors: { path: string; message: string }[];
  created_at: number;
}

const defined = (v: Num | undefined): v is number => typeof v === "number" && Number.isFinite(v);

/** A value already in percent: 12.5 -> "+12.50%". A dash, never a zero, for undefined. */
export function points(v: Num | undefined, precision = 2): string {
  return defined(v) ? `${v >= 0 ? "+" : ""}${v.toFixed(precision)}%` : "--";
}

export function metric(v: Num | undefined, precision = 2): string {
  return defined(v) ? v.toFixed(precision) : "--";
}

/** A share 0..1 as "37.5%", unsigned: a proportion, not a P&L. */
export function share(v: Num | undefined, precision = 1): string {
  return defined(v) ? `${(v * 100).toFixed(precision)}%` : "--";
}

/** Never "good idea": the grade is a statement about history, not advice. */
export function gradeWords(grade: string): string {
  switch (grade) {
    case "supported":
      return "SUPPORTED";
    case "fragile":
      return "FRAGILE";
    case "not_supported":
      return "NOT SUPPORTED";
    default:
      return "INSUFFICIENT EVIDENCE";
  }
}
