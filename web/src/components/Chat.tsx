"use client";

// The conversation: the user asks in plain English, the assistant fills the
// form behind the scenes (asking only what changes the answer), the run's
// Results appear in the conversation in the order the spec fixes -- verdict,
// the gates behind it, the stats -- and the assistant's analysis follows them,
// under a label saying an AI wrote it.
//
// The form is never in the way: a ready form shows as a short card with Run,
// and the form itself only if the user opens it.
import Link from "next/link";
import { useCallback, useEffect, useRef, useState } from "react";

import Markdown from "@/components/Markdown";
import ResultsView from "@/components/ResultsView";
import { readEvents, type Conversation, type Item, type Question, type TurnEvent } from "@/lib/conversation";
import type { Curve, JobStatus, Results } from "@/lib/results";

const TERMINAL = new Set(["done", "refused", "failed"]);

const EXAMPLES = [
  "What's the best drawdown to buy TQQQ at, selling when it gets back to its old high?",
  "Does buying SPY after a 20% drop beat holding it?",
  "If I bought QQQ every time it fell 30% from its high and held, how would that have done?",
];

type Live = Item & { live?: boolean };

export default function Chat({ id, onChange }: { id?: string; onChange?: () => void }) {
  const [conversationId, setConversationId] = useState(id);
  const [conversation, setConversation] = useState<Conversation | null>(null);
  const [items, setItems] = useState<Live[]>([]);
  const [activity, setActivity] = useState("");
  const [busy, setBusy] = useState(false);
  const [problem, setProblem] = useState("");
  const [draft, setDraft] = useState("");
  const bottom = useRef<HTMLDivElement>(null);
  const analysing = useRef(new Set<string>());

  const load = useCallback(async (which: string) => {
    const response = await fetch(`/api/conversations/${which}`);
    if (!response.ok) throw new Error(response.status === 404 ? "No such conversation." : `status ${response.status}`);
    const loaded = (await response.json()) as Conversation;
    setConversation(loaded);
    setItems(loaded.display);
  }, []);

  useEffect(() => {
    if (id) load(id).catch((e: Error) => setProblem(e.message));
  }, [id, load]);

  useEffect(() => {
    bottom.current?.scrollIntoView({ behavior: "smooth", block: "end" });
  }, [items, activity]);

  // One turn's events onto the screen. Text deltas grow the last item while it
  // is live; anything else closes it, so the next text starts a new one.
  const apply = useCallback((event: TurnEvent, kind: "assistant" | "analysis", jobId?: string) => {
    if (event.event === "activity") {
      setActivity(event.text);
      return;
    }
    if (event.event === "text") {
      setActivity("");
      setItems((prev) => {
        const last = prev[prev.length - 1];
        if (last?.live && (last.kind === "assistant" || last.kind === "analysis")) {
          return [...prev.slice(0, -1), { ...last, text: last.text + event.delta }];
        }
        const next: Live =
          kind === "analysis"
            ? { kind, text: event.delta, job_id: jobId ?? "", live: true }
            : { kind, text: event.delta, live: true };
        return [...prev, next];
      });
      return;
    }
    const close = (prev: Live[]) => prev.map((item) => (item.live ? { ...item, live: false } : item));
    if (event.event === "item") setItems((prev) => [...close(prev), event.item]);
    if (event.event === "error") setItems((prev) => [...close(prev), { kind: "error", text: event.message }]);
  }, []);

  const turn = useCallback(
    async (which: string, path: string, body: object, kind: "assistant" | "analysis", jobId?: string) => {
      setBusy(true);
      setProblem("");
      try {
        const response = await fetch(`/api/conversations/${which}/${path}`, {
          method: "POST",
          body: JSON.stringify(body),
        });
        await readEvents(response, (event) => apply(event, kind, jobId));
      } catch (e) {
        setProblem((e as Error).message);
      } finally {
        setActivity("");
        setBusy(false);
        await load(which).catch(() => undefined);
        onChange?.();
      }
    },
    [apply, load, onChange],
  );

  async function send(text: string) {
    const words = text.trim();
    if (!words || busy) return;
    setDraft("");
    let which = conversationId;
    if (!which) {
      const response = await fetch("/api/conversations", { method: "POST" });
      const body = await response.json();
      if (!response.ok) {
        setProblem(body.detail ?? body.error ?? `the service answered ${response.status}`);
        setDraft(words);
        return;
      }
      which = body.id as string;
      setConversationId(which);
      // The address follows the conversation without remounting the page.
      window.history.replaceState(null, "", `/c/${which}`);
    }
    await turn(which, "messages", { text: words }, "assistant");
  }

  async function run() {
    if (!conversationId || busy) return;
    setProblem("");
    const response = await fetch(`/api/conversations/${conversationId}/run`, { method: "POST" });
    const body = await response.json();
    if (response.status !== 202) {
      setProblem(body.error ?? body.detail ?? `the service answered ${response.status}`);
      return;
    }
    await load(conversationId);
    onChange?.();
  }

  // A run of this conversation has ended: the assistant reads its Results.
  const analyse = useCallback(
    (jobId: string) => {
      if (!conversationId || analysing.current.has(jobId)) return;
      analysing.current.add(jobId);
      turn(conversationId, "results", { job_id: jobId }, "analysis", jobId);
    },
    [conversationId, turn],
  );

  const lastReady = items.map((i) => i.kind).lastIndexOf("ready");
  const lastRun = items.map((i) => i.kind).lastIndexOf("run");

  return (
    <div className="chat">
      {items.length === 0 && !busy && (
        <div className="welcome">
          <h1>Ask about a trading rule</h1>
          <p className="dim">
            Describe a rule in plain English. The assistant turns it into a backtest, asks only what would change the
            answer, runs it on historical data, and shows you how it held up.
          </p>
          {EXAMPLES.map((example) => (
            <button key={example} className="example" onClick={() => send(example)}>
              {example}
            </button>
          ))}
        </div>
      )}

      {items.map((item, n) => {
        switch (item.kind) {
          case "user":
            return (
              <div key={n} className="bubble user">
                {item.text}
              </div>
            );
          case "assistant":
            return (
              <div key={n} className="bubble assistant">
                <Markdown text={item.text} />
              </div>
            );
          case "analysis":
            return (
              <div key={n} className="bubble assistant">
                <div className="heading">ANALYSIS - written by AI from the numbers above</div>
                <Markdown text={item.text} />
              </div>
            );
          case "error":
            return (
              <p key={n} className="error">
                ! {item.text}
              </p>
            );
          case "questions":
            return (
              <Questions
                key={n}
                message={item.message}
                questions={item.questions}
                answered={item.answered || busy || n !== items.length - 1}
                onAnswer={send}
              />
            );
          case "ready":
            return (
              <Ready
                key={n}
                form={item.form}
                runnable={n === lastReady && lastRun < n && !!conversation?.ready && !busy}
                onRun={run}
              />
            );
          case "run":
            return (
              <RunCard
                key={item.job_id}
                jobId={item.job_id}
                // A run the assistant started ends while its turn may still be
                // streaming; the Results wait for the turn, then go to it.
                analysed={!!conversation?.analysed.includes(item.job_id) || busy}
                onEnded={analyse}
              />
            );
        }
      })}

      {busy && <p className="activity">{activity || "Thinking..."}</p>}
      {problem && <p className="error">! {problem}</p>}
      <div ref={bottom} />

      <form
        className="composer"
        onSubmit={(e) => {
          e.preventDefault();
          send(draft);
        }}
      >
        <textarea
          value={draft}
          rows={2}
          placeholder={items.length ? "Reply, or ask for a change" : "A question about a trading rule"}
          onChange={(e) => setDraft(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter" && !e.shiftKey) {
              e.preventDefault();
              send(draft);
            }
          }}
        />
        <button className="primary" type="submit" disabled={busy || !draft.trim()}>
          Send
        </button>
      </form>
    </div>
  );
}

/** The assistant's questions, as buttons; one question answers on a click. */
function Questions({
  message,
  questions,
  answered,
  onAnswer,
}: {
  message?: string;
  questions: Question[];
  answered: boolean;
  onAnswer: (text: string) => void;
}) {
  const [chosen, setChosen] = useState<Record<number, string>>({});
  const [other, setOther] = useState<Record<number, string>>({});
  const single = questions.length === 1;
  const answer = (picks: Record<number, string>) =>
    onAnswer(questions.map((q, n) => (single ? picks[n] : `${q.question} ${picks[n]}`)).join("\n"));
  const complete = questions.every((_, n) => chosen[n] || other[n]?.trim());

  return (
    <div className={`card questions ${answered ? "answered" : ""}`}>
      {message && <Markdown text={message} />}
      {questions.map((q, n) => (
        <div key={n} className="question">
          <div>{q.question}</div>
          <div className="options">
            {q.options.map((option) => (
              <button
                key={option.label}
                disabled={answered}
                aria-pressed={chosen[n] === option.label}
                title={option.hint}
                onClick={() => {
                  const next = { ...chosen, [n]: option.label };
                  setChosen(next);
                  if (single) answer(next);
                }}
              >
                {option.label}
                {option.hint && <span className="hint">{option.hint}</span>}
              </button>
            ))}
          </div>
          {!answered && q.allow_free_text !== false && !single && (
            <input
              placeholder="or in your own words"
              value={other[n] ?? ""}
              onChange={(e) => {
                setOther({ ...other, [n]: e.target.value });
                setChosen({ ...chosen, [n]: "" });
              }}
            />
          )}
        </div>
      ))}
      {!answered && !single && (
        <button
          className="primary"
          disabled={!complete}
          onClick={() => answer(Object.fromEntries(questions.map((_, n) => [n, chosen[n] || other[n].trim()])))}
        >
          Answer
        </button>
      )}
      {!answered && single && <p className="faint">Or reply below in your own words.</p>}
    </div>
  );
}

/** A form the assistant has checked: Run, and the form for anyone who wants it. */
function Ready({ form, runnable, onRun }: { form: object; runnable: boolean; onRun: () => void }) {
  return (
    <div className="card ready">
      <div className="ready-row">
        <span>The backtest is set up.</span>
        {runnable && (
          <button className="primary" onClick={onRun}>
            Run it
          </button>
        )}
      </div>
      <details>
        <summary className="faint">Show the form</summary>
        <pre className="form-json">{JSON.stringify(form, null, 2)}</pre>
      </details>
    </div>
  );
}

/** A run: progress, then the Results; when it ends, the assistant is told. */
function RunCard({
  jobId,
  analysed,
  onEnded,
}: {
  jobId: string;
  /** Already read by the assistant, or not to be sent now. */
  analysed: boolean;
  onEnded: (jobId: string) => void;
}) {
  const [status, setStatus] = useState<JobStatus | null>(null);
  const [run, setRun] = useState<{ results: Results; curves: Curve[] } | null>(null);
  const [problem, setProblem] = useState("");

  useEffect(() => {
    let stopped = false;
    let timer: ReturnType<typeof setTimeout>;
    const poll = async () => {
      try {
        const response = await fetch(`/api/jobs/${jobId}`);
        if (!response.ok) throw new Error(`run ${jobId}: status ${response.status}`);
        const next = (await response.json()) as JobStatus;
        if (stopped) return;
        setStatus(next);
        if (next.state === "done") {
          const [results, curves] = await Promise.all([
            fetch(`/api/jobs/${jobId}/results`).then((r) => r.json()),
            fetch(`/api/jobs/${jobId}/curves`).then((r) => r.json()),
          ]);
          if (!stopped) setRun({ results, curves });
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
  }, [jobId]);

  const ended = !!status && TERMINAL.has(status.state);
  useEffect(() => {
    if (ended && !analysed) onEnded(jobId);
  }, [ended, analysed, jobId, onEnded]);

  return (
    <div className="card run">
      {problem && <p className="error">! {problem}</p>}
      {status && !ended && (
        <>
          <div>Running: {status.state === "queued" ? "queued" : status.stage || "starting"}</div>
          <div className="progress">
            <div style={{ width: `${status.total ? (100 * status.done) / status.total : 0}%` }} />
          </div>
          <p className="faint">No verdict is shown until every gate has been measured.</p>
        </>
      )}
      {status && (status.state === "refused" || status.state === "failed") && (
        <>
          <div className="serious">{status.state === "refused" ? "The job refused this form." : "The job failed."}</div>
          {status.errors.map((e) => (
            <p key={e.path + e.message} className="error">
              ! {e.path || "(engine)"}: {e.message}
            </p>
          ))}
        </>
      )}
      {run && (
        <>
          <ResultsView results={run.results} curves={run.curves} inConversation />
          <p className="faint">
            <Link href={`/runs/${jobId}`}>Open this run on its own page</Link>
          </p>
        </>
      )}
    </div>
  );
}
