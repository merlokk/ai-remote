"use client";

import { ChakraProvider } from "@chakra-ui/react";
import type { ReactNode } from "react";

import { BrowserKeyProvider } from "@/lib/browser-key-context";

import { system } from "./theme";

export function Providers({ children }: { children: ReactNode }) {
  return (
    <ChakraProvider value={system}>
      <BrowserKeyProvider>{children}</BrowserKeyProvider>
    </ChakraProvider>
  );
}
