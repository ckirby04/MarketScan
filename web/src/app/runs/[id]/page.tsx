"use client";

// One job: its progress while it runs, then its Results -- or, if it was
// refused, every reason by form path and a way back to the form.
//
// No partial verdict, ever: until the job is done nothing but progress shows,
// because a grade from half the gates is a different claim from the one the
// page would be making.
import Link from "next/link";
import { use, useEffect, useState } from "react";

import Rail from "@/components/Rail";
import ResultsView from "@/components/ResultsView";
import type { Curve, JobStatus, Results } from "@/lib/results";

const TERMINAL = new Set(["done", "refused", "failed"]);

export default function RunPage({ params }: { params: Promise<{ id: string }> }) {
  const { id } = use(params);
  const [status, setStatus] = useState<JobStatus | null>(null);
  const [run, setRun] = useState<{ results: Results; curves: Curve[] } | null>(null);
  const [problem, setProblem] = useState("");

  useEffect(() => {
    let stopped = false;
    let timer: ReturnType<typeof setTimeout>;
    const poll = async () => {
      try {
        const response = await fetch(`/api/jobs/${id}`);
        if (!response.ok) throw new Error(response.status === 404 ? "No such job." : `status ${response.status}`);
        const next = (await response.json()) as JobStatus;
        if (stopped) return;
        setStatus(next);
        if (next.state === "done") {
          const [results, curves] = await Promise.all([
            fetch(`/api/jobs/${id}/results`).then((r) => r.json()),
            fetch(`/api/jobs/${id}/curves`).then((r) => r.json()),
          ]);
          if (!stopped) setRun({ results, curves });
          return;
        }
        if (!TERMINAL.has(next.state)) timer = setTimeout(poll, 1000);
      } catch (e) {
        if (!stopped) setProblem((e as Error).message);
      }
    };
    poll();
    return () => {
      stopped = true;
      clearTimeout(timer);
    };
  }, [id]);

  return (
    <div className="shell">
      <Rail current={id} />
      <main className="main">
        {problem && <p className="error">! {problem}</p>}
        {status && !TERMINAL.has(status.state) && (
          <>
            <div className="heading">RUNNING</div>
            <p>{status.state === "queued" ? "queued" : status.stage || "starting"}</p>
            <div className="progress">
              <div style={{ width: `${status.total ? (100 * status.done) / status.total : 0}%` }} />
            </div>
            <p className="faint">No verdict is shown until every gate has been measured.</p>
            <button onClick={() => fetch(`/api/jobs/${id}/cancel`, { method: "POST" })}>Cancel</button>
          </>
        )}
        {status && (status.state === "refused" || status.state === "failed") && (
          <>
            <div className="heading">{status.state === "refused" ? "THE JOB REFUSED THIS FORM" : "THE JOB FAILED"}</div>
            {status.errors.map((e) => (
              <p key={e.path + e.message} className="error">
                ! {e.path || "(engine)"}: {e.message}
              </p>
            ))}
            <Link href={`/?from=${id}`}>Edit the form</Link>
          </>
        )}
        {run && (
          <>
            <ResultsView results={run.results} curves={run.curves} />
            <p>
              <Link href={`/?from=${id}`}>Edit the form</Link>
            </p>
          </>
        )}
      </main>
    </div>
  );
}
