"use client";

import Link from "next/link";
import { useEffect, useState } from "react";

import { gradeWords, type JobStatus } from "@/lib/results";

/** Asking, and recent runs. The assistant is not connected yet, and says so. */
export default function Rail({ current }: { current?: string }) {
  const [jobs, setJobs] = useState<JobStatus[] | null>(null);
  const [problem, setProblem] = useState("");

  useEffect(() => {
    fetch("/api/jobs")
      .then(async (r) => {
        if (!r.ok) throw new Error((await r.json()).error ?? `service answered ${r.status}`);
        return r.json() as Promise<JobStatus[]>;
      })
      .then(setJobs)
      .catch((e: Error) => setProblem(e.message));
  }, [current]);

  return (
    <aside className="rail">
      <div className="heading">ASK</div>
      <textarea disabled rows={3} style={{ width: "100%" }} placeholder="A question about a trading rule" />
      <p className="dim">
        The assistant is not connected yet (part R2). Fill in a form by hand; everything after the form works
        without it.
      </p>
      <Link href="/">Fill in a form by hand</Link>

      <div className="heading">RECENT RUNS</div>
      {problem && <p className="error">! {problem}</p>}
      {jobs?.length === 0 && <p className="faint">None yet.</p>}
      {jobs?.map((job) => (
        <Link key={job.job_id} href={`/runs/${job.job_id}`} className="recent">
          <span className={job.job_id === current ? "good" : ""}>{job.job_id}</span>
          <br />
          <span className="dim">
            {job.state === "done" && job.grade ? gradeWords(job.grade) : job.state}
          </span>{" "}
          <span className="faint">{new Date(job.created_at * 1000).toISOString().slice(0, 16).replace("T", " ")}</span>
        </Link>
      ))}
    </aside>
  );
}
