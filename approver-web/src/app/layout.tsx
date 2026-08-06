import type { Metadata } from "next";
import type { ReactNode } from "react";

import { Providers } from "./providers";

export const metadata: Metadata = {
  title: "approver-web",
  description: "Approve Claude Code permission requests over NATS",
};

export default function RootLayout({ children }: { children: ReactNode }) {
  return (
    <html lang="en" suppressHydrationWarning>
      {/* Extensions inject attributes here before React hydrates — Grammarly
          adds data-gr-ext-installed, others do the same — and every one of them
          is reported as a hydration mismatch we can neither cause nor fix.
          Suppressing on <body> covers exactly that, and nothing inside it. */}
      <body suppressHydrationWarning>
        <Providers>{children}</Providers>
      </body>
    </html>
  );
}
