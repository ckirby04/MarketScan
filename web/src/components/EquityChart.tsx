"use client";

// The equity chart: growth of 1 for the rule and its benchmark, and the
// drawdown strip under it, sharing its time axis.
//
// Plain SVG. Everything it plots is a series the engine produced -- normalised
// equity and drawdown come in the job's curves -- and the only arithmetic here
// is placing points on the page. Roles and colours follow the desktop chart: the
// rule in the first series slot, the benchmark in its own fixed token, one
// y-axis, no second scale.
import { useEffect, useRef, useState } from "react";

import type { Curve } from "@/lib/results";

const STRIP = 90;
const PAD = { left: 48, right: 12, top: 10, bottom: 22 };

function pathOf(xs: number[], ys: number[], x: (t: number) => number, y: (v: number) => number) {
  const step = Math.max(1, Math.floor(xs.length / 1500));
  let d = "";
  for (let i = 0; i < xs.length; i += step) {
    d += `${i === 0 ? "M" : "L"}${x(xs[i]).toFixed(1)},${y(ys[i]).toFixed(1)}`;
  }
  return d;
}

// Gridlines for a log axis: 1, 2, 5 per decade, or finer steps when the range
// spans too little of a decade for those to give at least three lines.
function logTicks(min: number, max: number): number[] {
  const within = (steps: number[]) => {
    const out: number[] = [];
    for (let e = Math.floor(Math.log10(min)) - 1; e <= Math.ceil(Math.log10(max)); e++) {
      for (const m of steps) {
        const v = m * 10 ** e;
        if (v >= min * (1 - 1e-9) && v <= max * (1 + 1e-9)) out.push(v);
      }
    }
    return out;
  };
  const coarse = within([1, 2, 5]);
  return coarse.length >= 3 ? coarse : within([1, 1.25, 1.5, 2, 2.5, 3, 4, 5, 6, 8]);
}

const growth = (v: number) => `${Number(v.toPrecision(3))}×`;

// Growth of 1 can reach 0 only for a total loss; the log axis stops just above it.
const FLOOR = 1e-4;

export default function EquityChart({ curves }: { curves: Curve[] }) {
  // Drawn at the width it is shown at, so its text stays readable on a phone
  // rather than being a scaled-down picture of a wide chart.
  const box = useRef<HTMLDivElement>(null);
  const [W, setW] = useState(900);
  useEffect(() => {
    const el = box.current;
    if (!el) return;
    const observer = new ResizeObserver(([entry]) => setW(Math.max(280, Math.round(entry.contentRect.width))));
    observer.observe(el);
    return () => observer.disconnect();
  }, []);
  const H = W < 600 ? 220 : 300;

  const subject = curves.find((c) => !c.is_benchmark && c.times.length > 1);
  const benchmark = curves.find((c) => c.is_benchmark && c.times.length > 1);
  const shown = [benchmark, subject].filter((c): c is Curve => Boolean(c));
  if (shown.length === 0) return <p className="faint" ref={box}>No curves for this run.</p>;

  const allTimes = shown.flatMap((c) => [c.times[0], c.times[c.times.length - 1]]);
  const t0 = Math.min(...allTimes);
  const t1 = Math.max(...allTimes);
  const values = shown.flatMap((c) => c.normalised);
  const v0 = Math.max(Math.min(...values), FLOOR);
  const v1 = Math.max(Math.max(...values), v0 * 1.01);
  const d0 = Math.min(...shown.flatMap((c) => c.drawdown), -0.01);

  // Log scale: equal heights are equal percentage moves, so a decade of slow
  // growth after a crash still shows its shape.
  const l0 = Math.log(v0);
  const l1 = Math.log(v1);
  const x = (t: number) => PAD.left + ((t - t0) / (t1 - t0 || 1)) * (W - PAD.left - PAD.right);
  const y = (v: number) => PAD.top + (1 - (Math.log(Math.max(v, FLOOR)) - l0) / (l1 - l0)) * (H - PAD.top - PAD.bottom);
  const yd = (v: number) => 6 + (v / d0) * (STRIP - 12);

  const years: number[] = [];
  for (let year = new Date(t0 * 1000).getUTCFullYear() + 1; ; year++) {
    const t = Date.UTC(year, 0, 1) / 1000;
    if (t > t1) break;
    years.push(t);
  }
  const yearStep = Math.max(1, Math.ceil(years.length / Math.max(2, Math.floor((W - PAD.left) / 64))));
  const colour = (c: Curve) => (c.is_benchmark ? "var(--benchmark)" : "var(--series-1)");

  return (
    <div ref={box}>
      <div className="faint chart-title">Growth of $1, log scale</div>
      <svg viewBox={`0 0 ${W} ${H}`} width="100%" role="img" aria-label="Growth of 1 on a log scale, rule and benchmark">
        <rect x={0} y={0} width={W} height={H} fill="var(--chart-surface)" />
        {logTicks(v0, v1).map((v) => (
          <g key={v}>
            <line
              x1={PAD.left}
              x2={W - PAD.right}
              y1={y(v)}
              y2={y(v)}
              stroke={v === 1 ? "var(--baseline)" : "var(--gridline)"}
            />
            <text x={PAD.left - 6} y={y(v) + 4} textAnchor="end" fontSize={11} fill="var(--text-faint)">
              {growth(v)}
            </text>
          </g>
        ))}
        {years
          .filter((_, i) => i % yearStep === 0)
          .map((t) => (
            <text key={t} x={x(t)} y={H - 6} textAnchor="middle" fontSize={11} fill="var(--text-faint)">
              {new Date(t * 1000).getUTCFullYear()}
            </text>
          ))}
        {shown.map((c) => (
          <path
            key={c.id}
            d={pathOf(c.times, c.normalised, x, y)}
            fill="none"
            stroke={colour(c)}
            strokeWidth={c.is_benchmark ? 1.4 : 2}
          />
        ))}
      </svg>
      <div className="faint chart-title" style={{ margin: "6px 0 2px" }}>
        Fall from the previous high
      </div>
      <svg viewBox={`0 0 ${W} ${STRIP}`} width="100%" role="img" aria-label="Drawdown, rule and benchmark">
        <rect x={0} y={0} width={W} height={STRIP} fill="var(--chart-surface)" />
        <line x1={PAD.left} x2={W - PAD.right} y1={yd(0)} y2={yd(0)} stroke="var(--baseline)" />
        <text x={PAD.left - 6} y={yd(0) + 4} textAnchor="end" fontSize={11} fill="var(--text-faint)">
          0%
        </text>
        <text x={PAD.left - 6} y={yd(d0) + 4} textAnchor="end" fontSize={11} fill="var(--text-faint)">
          {(d0 * 100).toFixed(0)}%
        </text>
        {shown.map((c) => (
          <path
            key={c.id}
            d={pathOf(c.times, c.drawdown, x, yd)}
            fill="none"
            stroke={colour(c)}
            strokeWidth={c.is_benchmark ? 1.2 : 1.6}
          />
        ))}
      </svg>
      <div className="faint">
        {shown.map((c) => (
          <span key={c.id} style={{ marginRight: 16 }}>
            <span style={{ color: colour(c) }}>&#9632;</span> {c.label}
            {c.is_benchmark ? " (benchmark)" : ""}
          </span>
        ))}
      </div>
    </div>
  );
}
