import { callService } from "@/lib/service";

// Recent jobs, newest first.
export async function GET() {
  return callService("/jobs");
}

// A Strategy Form in; the service validates it and answers 202 with a job, or
// 422 with every problem by form path.
export async function POST(request: Request) {
  return callService("/jobs", { method: "POST", body: await request.text() });
}
