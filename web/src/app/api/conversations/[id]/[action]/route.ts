import { callService } from "@/lib/service";

// An assistant turn can take minutes: several model calls, each with thinking.
export const maxDuration = 300;

// messages and results answer with a stream of events; run with JSON.
const ACTIONS = new Set(["messages", "run", "results"]);

export async function POST(request: Request, { params }: { params: Promise<{ id: string; action: string }> }) {
  const { id, action } = await params;
  if (!ACTIONS.has(action)) {
    return Response.json({ error: "not found" }, { status: 404 });
  }
  const body = action === "run" ? undefined : await request.text();
  return callService(`/conversations/${encodeURIComponent(id)}/${action}`, { method: "POST", body });
}
