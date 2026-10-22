import type { Metadata } from "next";
import { Inter } from "next/font/google";
import "./globals.css";

// The palette's sans face (SPEC_VIEW_MODES section 5), served with the app
// rather than left to whatever the reader's system has.
const inter = Inter({ subsets: ["latin"], variable: "--font-inter", display: "swap" });

export const metadata: Metadata = {
  title: "MarketScan Research",
  description: "Plain-English questions about trading rules, answered by graded backtests.",
};

// Light unless the reader chose dark. Set before first paint so a dark page
// never flashes light; Rail's toggle writes the same key.
const THEME = `try{if(localStorage.getItem("theme")==="dark")document.documentElement.dataset.theme="dark"}catch(e){}`;

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en" className={inter.variable} suppressHydrationWarning>
      <head>
        <script dangerouslySetInnerHTML={{ __html: THEME }} />
      </head>
      <body>{children}</body>
    </html>
  );
}
