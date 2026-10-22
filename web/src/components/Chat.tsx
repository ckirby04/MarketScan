"use client";

// The conversation: the user asks in plain English, the assistant fills the
// form behind the scenes (asking only what changes the answer), the run's
// Results appear in the conversation in the order the spec fixes -- verdict,
// the gates behind it, the stats -- and the assistant's analysis follows them,
// under a label saying an AI wrote it.
//
// The form never shows here: no fields, no JSON, no form paths. The user sees
// the conversation, a Run button when the backtest is set up, and the Results.
import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";

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

// `live`: text still streaming in. `sent`: the user's message as typed, shown
// until the service's own copy of it arrives.
type Live = Item & { live?: boolean; sent?: boolean };

/** Within this distance of the bottom, the reader is following along. */
const FOLLOW_PX = 80;

/**
 * The order things are shown in. A ready card goes after the assistant text
 * that follows it in the same turn -- what will run and what it assumes --
 * so Run comes after the explanation rather than before it.
 */
function displayOrder(items: Item[]): number[] {
  const order: number[] = [];
  for (let n = 0; n < items.length; n++) {
    if (items[n].kind !== "ready") {
      order.push(n);
      continue;
    }
    let end = n + 1;
    while (end < items.length && items[end].kind === "assistant") order.push(end++);
    order.push(n);
    n = end - 1;
  }
  return order;
}

export default function Chat({ id, onChange }: { id?: string; onChange?: () => void }) {
  const [conversationId, setConversationId] = useState(id);
  const [conversation, setConversation] = useState<Conversation | null>(null);
  const [items, setItems] = useState<Live[]>([]);
  const [activity, setActivity] = useState("");
  const [busy, setBusy] = useState(false);
  const [problem, setProblem] = useState("");
  const [draft, setDraft] = useState("");
  // Run was pressed and the service has not answered yet.
  const [starting, setStarting] = useState(false);
  const analysing = useRef(new Set<string>());
  // Whether new content should bring the page down with it. Only a scroll by
  // the reader changes it: content growing below them does not.
  const following = useRef(true);

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
    const onScroll = () => {
      const root = document.documentElement;
      following.current = window.innerHeight + window.scrollY >= root.scrollHeight - FOLLOW_PX;
    };
    window.addEventListener("scroll", onScroll, { passive: true });
    return () => window.removeEventListener("scroll", onScroll);
  }, []);

  // Instant, not smooth: a smooth scroll's own scroll events would read as the
  // reader leaving the bottom halfway down.
  useEffect(() => {
    if (following.current) window.scrollTo({ top: document.documentElement.scrollHeight });
  }, [items, activity, busy]);

  /** A run has its Results: show them from the verdict down, and stop following. */
  const showResults = useCallback((card: HTMLElement) => {
    following.current = false;
    card.scrollIntoView({ behavior: "smooth", block: "start" });
  }, []);

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
    if (event.event === "item") {
      // The service's copy of the message on screen replaces the page's.
      const echo = event.item.kind === "user";
      setItems((prev) => [...close(prev).filter((item) => !(echo && item.sent)), event.item]);
    }
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
    // On screen at once, and busy from now: creating the conversation and the
    // first event of the turn can each take a few seconds.
    following.current = true;
    setItems((prev) => [...prev, { kind: "user", text: words, sent: true }]);
    setBusy(true);
    let which = conversationId;
    if (!which) {
      const response = await fetch("/api/conversations", { method: "POST" });
      const body = await response.json();
      if (!response.ok) {
        setProblem(body.detail ?? body.error ?? `the service answered ${response.status}`);
        setItems((prev) => prev.filter((item) => !item.sent));
        setBusy(false);
        setDraft(words);
        return;
      }
      which = body.id as string;
      setConversationId(which);
      // The address follows the conversation without remounting the page.
      window.history.replaceState(null, "", `/c/${which}`);
      onChange?.();
    }
    await turn(which, "messages", { text: words }, "assistant");
  }

  async function run() {
    if (!conversationId || busy || starting) return;
    following.current = true;
    setProblem("");
    setStarting(true);
    try {
      const response = await fetch(`/api/conversations/${conversationId}/run`, { method: "POST" });
      const body = await response.json();
      if (response.status !== 202) {
        setProblem(body.error ?? body.detail ?? `the service answered ${response.status}`);
        return;
      }
      await load(conversationId);
      onChange?.();
    } finally {
      setStarting(false);
    }
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
  const loading = !!id && conversation === null && !problem;
  const empty = items.length === 0 && !busy && !loading;

  const composer = (
    <Composer
      value={draft}
      onChange={setDraft}
      onSend={() => send(draft)}
      busy={busy}
      placeholder={empty ? "Ask about a trading rule" : "Reply"}
      autoFocus={empty}
    />
  );

  if (empty) {
    return (
      <div className="chat empty">
        <div className="welcome">
          <h1>What do you want to test?</h1>
          {composer}
          {problem && <p className="notice">{problem}</p>}
          <div className="examples">
            {EXAMPLES.map((example) => (
              <button key={example} className="example" onClick={() => send(example)}>
                {example}
              </button>
            ))}
          </div>
        </div>
      </div>
    );
  }

  return (
    <div className="chat">
      <div className="thread">
        {loading && <p className="activity">Loading the conversation…</p>}

        {displayOrder(items).map((n) => {
          const item = items[n];
          switch (item.kind) {
            case "user":
              return (
                <div key={n} className="message user">
                  {item.text}
                </div>
              );
            case "assistant":
              return (
                <div key={n} className="message assistant">
                  <Markdown text={item.text} />
                </div>
              );
            case "analysis":
              return (
                <div key={n} className="message assistant">
                  <div className="caption">Analysis · written by AI from the results above</div>
                  <Markdown text={item.text} />
                </div>
              );
            case "error":
              return (
                <p key={n} className="notice">
                  {item.text}
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
              // Only the one that can still be run: an older or already-run
              // setup has nothing left to offer.
              return n === lastReady && lastRun < n && !!conversation?.ready && !busy ? (
                <div key={n} className="run-offer">
                  <button className="primary" onClick={run} disabled={starting}>
                    {starting ? "Starting…" : "Run the backtest"}
                  </button>
                  {!starting && <span className="faint">or reply to change something first</span>}
                </div>
              ) : null;
            case "run":
              return (
                <RunCard
                  key={item.job_id}
                  jobId={item.job_id}
                  // A run the assistant started ends while its turn may still be
                  // streaming; the Results wait for the turn, then go to it.
                  analysed={!!conversation?.analysed.includes(item.job_id) || busy}
                  onEnded={analyse}
                  onResults={showResults}
                />
              );
          }
        })}

        {busy && <Thinking text={activity} />}
        {problem && <p className="notice">{problem}</p>}
      </div>
      <div className="dock">{composer}</div>
    </div>
  );
}

/** The message box: grows with its text, Enter sends, Shift+Enter is a new line. */
function Composer({
  value,
  onChange,
  onSend,
  busy,
  placeholder,
  autoFocus,
}: {
  value: string;
  onChange: (value: string) => void;
  onSend: () => void;
  busy: boolean;
  placeholder: string;
  autoFocus?: boolean;
}) {
  const box = useRef<HTMLTextAreaElement>(null);
  useLayoutEffect(() => {
    const el = box.current;
    if (!el) return;
    el.style.height = "auto";
    el.style.height = `${Math.min(el.scrollHeight, 220)}px`;
  }, [value]);

  return (
    <form
      className="composer"
      onSubmit={(e) => {
        e.preventDefault();
        onSend();
      }}
    >
      <textarea
        ref={box}
        value={value}
        rows={1}
        autoFocus={autoFocus}
        placeholder={placeholder}
        onChange={(e) => onChange(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === "Enter" && !e.shiftKey) {
            e.preventDefault();
            onSend();
          }
        }}
      />
      <button className="send" type="submit" disabled={busy || !value.trim()} aria-label="Send">
        <svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
          <path d="M8 13V3M3.5 7.5 8 3l4.5 4.5" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" />
        </svg>
      </button>
    </form>
  );
}

/** What the assistant is doing, while it works. */
function Thinking({ text }: { text: string }) {
  return (
    <div className="activity" role="status">
      <span className="dots" aria-hidden="true">
        <span />
        <span />
        <span />
      </span>
      {text || "Thinking"}
    </div>
  );
}

/**
 * The assistant's questions, as buttons; one question answers on a click.
 * Once answered, only the message stays: the reply below it says what was chosen.
 */
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

  if (answered) {
    return message ? (
      <div className="message assistant">
        <Markdown text={message} />
      </div>
    ) : null;
  }

  return (
    <div className="message assistant">
      {message && <Markdown text={message} />}
      <div className="questions">
        {questions.map((q, n) => (
          <div key={n} className="question">
            <div className="question-text">{q.question}</div>
            <div className="options">
              {q.options.map((option) => (
                <button
                  key={option.label}
                  aria-pressed={chosen[n] === option.label}
                  onClick={() => {
                    const next = { ...chosen, [n]: option.label };
                    setChosen(next);
                    setOther({ ...other, [n]: "" });
                    if (single) answer(next);
                  }}
                >
                  <span className="option-label">{option.label}</span>
                  {option.hint && <span className="hint">{option.hint}</span>}
                </button>
              ))}
            </div>
            {q.allow_free_text !== false && !single && (
              <input
                placeholder="Something else…"
                value={other[n] ?? ""}
                onChange={(e) => {
                  setOther({ ...other, [n]: e.target.value });
                  setChosen({ ...chosen, [n]: "" });
                }}
              />
            )}
          </div>
        ))}
        {!single && (
          <button
            className="primary"
            disabled={!complete}
            onClick={() => answer(Object.fromEntries(questions.map((_, n) => [n, chosen[n] || other[n].trim()])))}
          >
            Continue
          </button>
        )}
      </div>
    </div>
  );
}

/** A run: progress, then the Results; when it ends, the assistant is told. */
function RunCard({
  jobId,
  analysed,
  onEnded,
  onResults,
}: {
  jobId: string;
  /** Already read by the assistant, or not to be sent now. */
  analysed: boolean;
  onEnded: (jobId: string) => void;
  /** Called once, when Results arrive for a run this page watched. */
  onResults: (card: HTMLElement) => void;
}) {
  const [status, setStatus] = useState<JobStatus | null>(null);
  const [run, setRun] = useState<{ results: Results; curves: Curve[] } | null>(null);
  const [problem, setProblem] = useState("");
  const card = useRef<HTMLDivElement>(null);
  // Whether this page saw the run in progress; a finished run reopened later
  // does not move the page.
  const watched = useRef(false);

  useEffect(() => {
    let stopped = false;
    let timer: ReturnType<typeof setTimeout>;
    const poll = async () => {
      try {
        const response = await fetch(`/api/jobs/${jobId}`);
        if (!response.ok) throw new Error(`status ${response.status}`);
        const next = (await response.json()) as JobStatus;
        if (stopped) return;
        setStatus(next);
        if (!TERMINAL.has(next.state)) watched.current = true;
        if (next.state === "done") {
          const [results, curves] = await Promise.all([
            fetch(`/api/jobs/${jobId}/results`).then((r) => r.json()),
            fetch(`/api/jobs/${jobId}/curves`).then((r) => r.json()),
          ]);
          if (!stopped) setRun({ results, curves });
        }
        if (!TERMINAL.has(next.state)) timer = setTimeout(poll, 1000);
      } catch (e) {
        if (!stopped) setProblem(`Couldn't load this backtest (${(e as Error).message}).`);
      }
    };
    poll();
    return () => {
      stopped = true;
      clearTimeout(timer);
    };
  }, [jobId]);

  useEffect(() => {
    if (run && watched.current && card.current) onResults(card.current);
  }, [run, onResults]);

  const ended = !!status && TERMINAL.has(status.state);
  useEffect(() => {
    if (ended && !analysed) onEnded(jobId);
  }, [ended, analysed, jobId, onEnded]);

  return (
    <div ref={card} className="card run">
      {problem && <p className="notice">{problem}</p>}
      {!status && !problem && <Thinking text="Starting the backtest" />}
      {status && !ended && (
        <>
          <div className="run-stage">
            Running the backtest
            <span className="faint"> · {status.state === "queued" ? "waiting to start" : status.stage || "starting"}</span>
          </div>
          <div className="progress">
            <div style={{ width: `${status.total ? (100 * status.done) / status.total : 0}%` }} />
          </div>
          <p className="faint">The verdict appears once every check has finished.</p>
        </>
      )}
      {status && (status.state === "refused" || status.state === "failed") && (
        <>
          <p className="notice">The backtest couldn&apos;t run. The assistant will explain what to change.</p>
          {status.errors.length > 0 && (
            <details>
              <summary className="faint">Technical details</summary>
              {status.errors.map((e) => (
                <p key={e.path + e.message} className="faint mono">
                  {e.message}
                </p>
              ))}
            </details>
          )}
        </>
      )}
      {run && <ResultsView results={run.results} curves={run.curves} inConversation />}
    </div>
  );
}
