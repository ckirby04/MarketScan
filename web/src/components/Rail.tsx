"use client";

import Link from "next/link";
import { useEffect, useState } from "react";

import type { ConversationSummary } from "@/lib/conversation";

/**
 * New chat, and recent conversations. `version` changes when the page has done
 * something that may have added to the list, and it is read again. On a narrow
 * screen it is a drawer behind the menu button.
 */
export default function Rail({ current, version = 0 }: { current?: string; version?: number }) {
  const [conversations, setConversations] = useState<ConversationSummary[] | null>(null);
  const [problem, setProblem] = useState("");
  const [open, setOpen] = useState(false);

  useEffect(() => {
    fetch("/api/conversations")
      .then(async (r) => (r.ok ? (r.json() as Promise<ConversationSummary[]>) : Promise.reject(new Error(`status ${r.status}`))))
      .then((list) => {
        setConversations(list);
        setProblem("");
      })
      .catch(() => setProblem("Couldn't load your conversations."));
  }, [current, version]);

  return (
    <>
      <button className="menu" aria-label="Conversations" aria-expanded={open} onClick={() => setOpen(!open)}>
        <svg viewBox="0 0 16 16" width="18" height="18" aria-hidden="true">
          <path d="M2.5 4h11M2.5 8h11M2.5 12h11" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
        </svg>
      </button>
      {open && <div className="scrim" onClick={() => setOpen(false)} />}
      <aside className={`rail ${open ? "open" : ""}`}>
        {/* A full load, not a client navigation: from a conversation that began on
            this same page, "/" would otherwise keep the old one on screen. */}
        {/* eslint-disable-next-line @next/next/no-html-link-for-pages */}
        <a href="/" className="new">
          <svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true">
            <path d="M8 3v10M3 8h10" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" />
          </svg>
          New chat
        </a>

        <div className="rail-label">Recents</div>
        {problem && <p className="faint rail-note">{problem}</p>}
        {conversations?.length === 0 && <p className="faint rail-note">Your conversations will show here.</p>}
        <nav>
          {conversations?.map((c) => (
            <Link
              key={c.id}
              href={`/c/${c.id}`}
              className={`recent ${c.id === current ? "current" : ""}`}
              title={c.title}
              onClick={() => setOpen(false)}
            >
              {c.title || "New conversation"}
            </Link>
          ))}
        </nav>
      </aside>
    </>
  );
}
