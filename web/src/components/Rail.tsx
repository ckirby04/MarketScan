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
  const [dark, setDark] = useState(false);

  // The layout set data-theme before paint; read it back for the toggle.
  useEffect(() => setDark(document.documentElement.dataset.theme === "dark"), []);

  function toggleTheme() {
    const next = !dark;
    setDark(next);
    if (next) document.documentElement.dataset.theme = "dark";
    else delete document.documentElement.dataset.theme;
    try {
      localStorage.setItem("theme", next ? "dark" : "light");
    } catch {
      // private browsing: the choice lasts for this page only
    }
  }

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
        <div className="brand">
          <span className="brand-mark" aria-hidden="true">
            <svg viewBox="0 0 16 16" width="14" height="14">
              <path d="M2.5 11.5 6 7.5l2.5 2.5 5-6" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" />
            </svg>
          </span>
          MarketScan
        </div>
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

        <div className="rail-foot">
          <button className="theme" onClick={toggleTheme} aria-pressed={dark}>
            <svg viewBox="0 0 16 16" width="15" height="15" aria-hidden="true">
              {dark ? (
                <path d="M8 1.5v1.5M8 13v1.5M1.5 8H3M13 8h1.5M3.4 3.4l1 1M11.6 11.6l1 1M3.4 12.6l1-1M11.6 4.4l1-1M8 5a3 3 0 1 0 0 6 3 3 0 0 0 0-6Z" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" />
              ) : (
                <path d="M13.5 9.5A5.5 5.5 0 0 1 6.5 2.5a5.5 5.5 0 1 0 7 7Z" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinejoin="round" />
              )}
            </svg>
            {dark ? "Light mode" : "Dark mode"}
          </button>
        </div>
      </aside>
    </>
  );
}
