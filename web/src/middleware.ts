import { NextResponse, type NextRequest } from "next/server";

// Personal use: one password, over HTTP Basic auth, from WEB_PASSWORD. Unset
// means open, which is right on a developer's machine and wrong anywhere else,
// so a deployment without it answers 503 rather than serving the app.
export function middleware(request: NextRequest) {
  const password = process.env.WEB_PASSWORD;
  if (!password) {
    if (process.env.VERCEL) {
      return new NextResponse("WEB_PASSWORD is not set for this deployment.", { status: 503 });
    }
    return NextResponse.next();
  }
  const header = request.headers.get("authorization") ?? "";
  const [scheme, encoded] = header.split(" ");
  if (scheme === "Basic" && encoded) {
    const decoded = atob(encoded);
    const supplied = decoded.slice(decoded.indexOf(":") + 1);
    if (supplied === password) {
      return NextResponse.next();
    }
  }
  return new NextResponse("Authentication required.", {
    status: 401,
    headers: { "WWW-Authenticate": 'Basic realm="MarketScan"' },
  });
}

// Node.js rather than the default Edge runtime: Vercel services deploy no Edge
// functions, and refuse a service whose build produces one.
export const config = {
  runtime: "nodejs",
  matcher: ["/((?!_next/static|_next/image|favicon.ico).*)"],
};
