"use client";

/**
 * The browser-held key, shared by the register panel and every decision form.
 *
 * A context rather than two independent IndexedDB readers, so that registering
 * in the panel immediately unlocks the buttons on the cards below it.
 */
import { createContext, type ReactNode, useCallback, useContext, useEffect, useState } from "react";

import { generateKey, loadStoredKey, signBytes, type StoredKey, storeKey } from "./browser-key";
import { type Decision, decisionSigningBytes } from "./reply";
import type { PermissionRequest } from "./schemas";
import type { RegisterResult } from "./types";

interface BrowserKeyValue {
  /** null while loading, or when this browser holds no key. */
  key: StoredKey | null;
  /** False until IndexedDB has been read — avoids a "no key" flash on load. */
  ready: boolean;
  register(token: string): Promise<RegisterResult>;
  sign(request: PermissionRequest, decision: Decision): Promise<string>;
}

const Ctx = createContext<BrowserKeyValue | null>(null);

export function BrowserKeyProvider({ children }: { children: ReactNode }) {
  const [key, setKey] = useState<StoredKey | null>(null);
  const [ready, setReady] = useState(false);

  useEffect(() => {
    let live = true;
    loadStoredKey().then((record) => {
      if (!live) return;
      setKey(record);
      setReady(true);
    });
    return () => {
      live = false;
    };
  }, []);

  const register = useCallback(async (token: string): Promise<RegisterResult> => {
    const fresh = await generateKey();
    const response = await fetch("/api/register", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        token,
        public_key: fresh.publicB64,
        public_key_raw: fresh.publicRawB64,
      }),
    });
    const body = (await response.json()) as RegisterResult;
    if (!body.ok) return body; // rejected: the existing key is untouched

    // Persist only now — same ordering as both Python responders.
    try {
      const record: StoredKey = {
        id: "responder",
        key_id: body.key_id ?? token.split(".", 1)[0]!,
        public_key: fresh.publicB64,
        privateKey: fresh.privateKey,
        created_ts: Date.now(),
      };
      await storeKey(record);
      setKey(record);
    } catch (err) {
      return {
        ok: false,
        error:
          `registered, but this browser could not store the key (${
            err instanceof Error ? err.message : String(err)
          }). The token is spent — mint another one and register again.`,
      };
    }
    return body;
  }, []);

  const sign = useCallback(
    async (request: PermissionRequest, decision: Decision) => {
      if (!key) throw new Error("this browser holds no signing key");
      return signBytes(key.privateKey, await decisionSigningBytes(request, decision));
    },
    [key],
  );

  return <Ctx.Provider value={{ key, ready, register, sign }}>{children}</Ctx.Provider>;
}

export function useBrowserKey(): BrowserKeyValue {
  const value = useContext(Ctx);
  if (!value) throw new Error("useBrowserKey outside BrowserKeyProvider");
  return value;
}

