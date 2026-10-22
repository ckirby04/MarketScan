// The job service, called from this app's server and never from a browser.
//
// The service's key lives in RESEARCH_SERVICE_KEY on Vercel and is attached
// here; a route handler forwards the service's answer, status code and all.

export function serviceConfigured(): boolean {
  return Boolean(process.env.RESEARCH_SERVICE_URL && process.env.RESEARCH_SERVICE_KEY);
}

export async function callService(path: string, init: RequestInit = {}): Promise<Response> {
  const base = process.env.RESEARCH_SERVICE_URL;
  const key = process.env.RESEARCH_SERVICE_KEY;
  if (!base || !key) {
    return Response.json(
      { error: "RESEARCH_SERVICE_URL and RESEARCH_SERVICE_KEY are not set on the web app." },
      { status: 503 },
    );
  }
  const upstream = await fetch(`${base.replace(/\/$/, "")}${path}`, {
    ...init,
    headers: {
      ...(init.headers ?? {}),
      Authorization: `Bearer ${key}`,
      ...(init.body ? { "Content-Type": "application/json" } : {}),
    },
    cache: "no-store",
  });
  const contentType = upstream.headers.get("Content-Type") ?? "application/json";
  // An assistant turn is a stream of NDJSON events, passed on as they arrive
  // rather than read whole: the browser shows the reply while it is written.
  if (contentType.startsWith("application/x-ndjson") && upstream.body) {
    return new Response(upstream.body, {
      status: upstream.status,
      headers: { "Content-Type": contentType, "Cache-Control": "no-store", "X-Accel-Buffering": "no" },
    });
  }
  const body = await upstream.text();
  return new Response(body, { status: upstream.status, headers: { "Content-Type": contentType } });
}
