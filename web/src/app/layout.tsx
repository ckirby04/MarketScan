import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "MarketScan Research",
  description: "Plain-English questions about trading rules, answered by graded backtests.",
};

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
