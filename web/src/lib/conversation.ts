// A conversation with the assistant, as the service shows it (service/app/assistant.py).
//
// The service keeps the model's history; the page gets only `display`, the
// things the user sees, in order. A turn arrives as NDJSON events and the page
// reloads the conversation when it ends, so what is on screen after a turn is
// always the service's record, not the page's reconstruction of it.

import type { JsonObject } from "@/lib/json";

export interface QuestionOption {
  label: string;
  value: unknown;
  hint?: string;
}

export interface Question {
  path: string;
  question: string;
  options: QuestionOption[];
  allow_free_text?: boolean;
}

export type Item =
  | { kind: "user"; text: string }
  | { kind: "assistant"; text: string }
  | { kind: "analysis"; text: string; job_id: string }
  | { kind: "questions"; questions: Question[]; answered: boolean }
  | { kind: "ready"; form: JsonObject }
  | { kind: "run"; job_id: string }
  | { kind: "error"; text: string };

export interface Conversation {
  id: string;
  title: string;
  created_at: number;
  updated_at: number;
  display: Item[];
  form: JsonObject | null;
  ready: boolean;
  runs: string[];
  analysed: string[];
}

export interface ConversationSummary {
  id: string;
  title: string;
  updated_at: number;
}

export type TurnEvent =
  | { event: "text"; delta: string }
  | { event: "activity"; text: string }
  | { event: "item"; item: Item }
  | { event: "form"; form: JsonObject; ready: boolean }
  | { event: "error"; message: string }
  | { event: "end" };

/** Reads an NDJSON response, calling `onEvent` for each line as it arrives. */
export async function readEvents(response: Response, onEvent: (event: TurnEvent) => void): Promise<void> {
  if (!response.ok || !response.body) {
    let message = `the service answered ${response.status}`;
    try {
      const body = await response.json();
      message = body.detail ?? body.error ?? message;
    } catch {
      // not JSON: keep the status
    }
    throw new Error(message);
  }
  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  for (;;) {
    const { done, value } = await reader.read();
    buffer += decoder.decode(value ?? new Uint8Array(), { stream: !done });
    let newline;
    while ((newline = buffer.indexOf("\n")) >= 0) {
      const line = buffer.slice(0, newline).trim();
      buffer = buffer.slice(newline + 1);
      if (line) onEvent(JSON.parse(line) as TurnEvent);
    }
    if (done) break;
  }
  if (buffer.trim()) onEvent(JSON.parse(buffer) as TurnEvent);
}
