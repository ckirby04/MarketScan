import { callService } from "@/lib/service";

// Recent conversations with the assistant, newest first.
export async function GET() {
  return callService("/conversations");
}

// A new, empty conversation.
export async function POST() {
  return callService("/conversations", { method: "POST" });
}
