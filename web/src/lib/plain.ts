// Plain words for the Results' identifiers: gate ids, warning codes, swept
// parameter paths, objective metrics and sub-grades, so the page never shows a
// schema name.
//
// The words themselves are contracts/research/plain_words.json, which the
// desktop app reads too (apps/marketscan/plain_words.cpp); this file only looks
// them up. Every number shown next to them still comes from the Results, and
// the engine's own text stays available beside the plain one.
import words from "@/contracts/plain_words.json";

type Entry<T> = Record<string, T | undefined>;

/** Each gate: a short name, and what it asks of the rule. */
export const GATES: Entry<{ name: string; asks: string }> = words.gates;

/** The scorecard's sub-grades (JOB.md section 11): a name, and what each reads. */
export const SUBGRADES: Entry<{ name: string; reads: string }> = words.subgrades;

const WARNINGS: Entry<string> = words.warnings;
const PARAMS: Entry<{ label: string; unit: string }> = words.params;
const OBJECTIVES: Entry<string> = words.objectives;

export function gateName(id: string): string {
  return GATES[id]?.name ?? id.replace(/_/g, " ");
}

/** A code not listed falls back to the engine's message. */
export function warningText(code: string, message: string): string {
  return WARNINGS[code] ?? message;
}

const lastPart = (path: string) => path.slice(path.lastIndexOf("/") + 1);

/** A swept parameter, by the last part of its path. */
export function paramLabel(path: string): string {
  const key = lastPart(path);
  return PARAMS[key]?.label ?? key.replace(/_/g, " ");
}

/** A parameter's value with its unit: 20 -> "20%", 252 -> "252 days". */
export function paramValue(path: string, value: number): string {
  const key = lastPart(path);
  const unit = PARAMS[key]?.unit ?? (key.endsWith("_pct") ? "%" : key.endsWith("days") ? "days" : "");
  const shown = plainNumber(value);
  return unit === "%" ? `${shown}%` : unit ? `${shown} ${unit}` : shown;
}

/** What "best" meant when the settings were ranked. */
export function objectiveLabel(metric: string): string {
  return OBJECTIVES[metric] ?? metric.replace(/_/g, " ");
}

/** A number without trailing zeros: 20 -> "20", 2.5 -> "2.5". Formatting only. */
export function plainNumber(v: number): string {
  return String(Number(v.toFixed(4)));
}
