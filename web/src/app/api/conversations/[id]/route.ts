import { callService } from "@/lib/service";

// What the user sees of a conversation: its messages, questions, runs.
export async function GET(_request: Request, { params }: { params: Promise<{ id: string }> }) {
  const { id } = await params;
  return callService(`/conversations/${encodeURIComponent(id)}`);
}
