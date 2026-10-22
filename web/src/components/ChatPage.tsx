"use client";

import { useCallback, useState } from "react";

import Chat from "@/components/Chat";
import Rail from "@/components/Rail";

/** The home page and a conversation's page: the rail, and the chat. */
export default function ChatPage({ id }: { id?: string }) {
  const [version, setVersion] = useState(0);
  const changed = useCallback(() => setVersion((v) => v + 1), []);
  return (
    <div className="shell">
      <Rail current={id} version={version} />
      <main className="main conversation">
        <Chat id={id} onChange={changed} />
      </main>
    </div>
  );
}
