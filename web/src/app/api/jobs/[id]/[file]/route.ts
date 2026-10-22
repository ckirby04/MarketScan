import { callService } from "@/lib/service";

// Only the run files the service serves; anything else is not forwarded.
const FILES = new Set(["results", "curves", "form"]);

export async function GET(
  _request: Request,
  { params }: { params: Promise<{ id: string; file: string }> },
) {
  const { id, file } = await params;
  if (!FILES.has(file)) {
    return Response.json({ error: "not found" }, { status: 404 });
  }
  return callService(`/jobs/${encodeURIComponent(id)}/${file}`);
}

export async function POST(
  _request: Request,
  { params }: { params: Promise<{ id: string; file: string }> },
) {
  const { id, file } = await params;
  if (file !== "cancel") {
    return Response.json({ error: "not found" }, { status: 404 });
  }
  return callService(`/jobs/${encodeURIComponent(id)}/cancel`, { method: "POST" });
}
