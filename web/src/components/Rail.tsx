"use client";

import Link from "next/link";
import { useEffect, useState } from "react";

import type { ConversationSummary } from "@/lib/conversation";
import { gradeWords, type JobStatus } from "@/lib/results";

async function list<T>(path: string): Promise<T[]> {
  const r = await fetch(path);
  if (!r.ok) throw new Error((await r.json()).error ?? `service answered ${r.status}`);
  return r.json() as Promise<T[]>;
}

const when = (seconds: number) => new Date(seconds * 1000).toISOString().slice(0, 16).replace("T", " ");

/**
 * Conversations and recent runs. `version` changes when the page has done
 * something that may have added to either, and the lists are read again.
 */
export default function Rail({ current, version = 0 }: { current?: string; version?: number }) {
  const [conversations, setConversations] = useState<ConversationSummary[] | null>(null);
  const [jobs, setJobs] = useState<JobStatus[] | null>(null);
  const [problem, setProblem] = useState("");

  useEffect(() => {
    list<ConversationSummary>("/api/conversations")
      .then(setConversations)
      .catch((e: Error) => setProblem(e.message));
    list<JobStatus>("/api/jobs")
      .then(setJobs)
      .catch((e: Error) => setProblem(e.message));
  }, [current, version]);

  return (
    <aside className="rail">
      {/* A full load, not a client navigation: from a conversation that began on
          this same page, "/" would otherwise keep the old one on screen. */}
      {/* eslint-disable-next-line @next/next/no-html-link-for-pages */}
      <a href="/" className="new">
        + New question
      </a>
      {problem && <p className="error">! {problem}</p>}

      <div className="heading">CONVERSATIONS</div>
      {conversations?.length === 0 && <p className="faint">None yet.</p>}
      {conversations?.map((c) => (
        <Link key={c.id} href={`/c/${c.id}`} className="recent">
          <span className={c.id === current ? "good" : ""}>{c.title || "(untitled)"}</span>
          <br />
          <span className="faint">{when(c.updated_at)}</span>
        </Link>
      ))}

      <div className="heading">RECENT RUNS</div>
      {jobs?.length === 0 && <p className="faint">None yet.</p>}
      {jobs?.slice(0, 12).map((job) => (
        <Link key={job.job_id} href={`/runs/${job.job_id}`} className="recent">
          <span className={job.job_id === current ? "good" : ""}>{job.job_id}</span>
          <br />
          <span className="dim">{job.state === "done" && job.grade ? gradeWords(job.grade) : job.state}</span>{" "}
          <span className="faint">{when(job.created_at)}</span>
        </Link>
      ))}

      <p className="faint" style={{ marginTop: 24 }}>
        <Link href="/form">Fill in a form by hand</Link>
      </p>
    </aside>
  );
}
