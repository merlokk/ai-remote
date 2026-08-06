"use client";

/**
 * Notice a *new* request: chirp, and put the count in the tab title.
 *
 * Two rules that keep it from crying wolf:
 *
 *  - **The first snapshot never sounds.** Opening the page with four requests
 *    already waiting is not four new events, and a beep on every reload trains
 *    you to ignore it.
 *  - **Only genuinely new nonces sound.** Every SSE frame is a whole snapshot,
 *    so "new" has to be diffed rather than counted — a request expiring while
 *    another arrives keeps the length the same.
 *
 * The tab title carries the count too, because the reason this exists is that
 * the operator is looking at something else.
 */
import { useCallback, useEffect, useRef, useState } from "react";

import { audioState, audioSupported, enableAudio, playAlert } from "./alert-sound";
import type { PendingRequest } from "./types";

const STORAGE_KEY = "approver-web:sound";
const BASE_TITLE = "approver-web";

export type SoundState = "on" | "off" | "blocked" | "unsupported";

export function useRequestAlert(requests: PendingRequest[] | null): {
  sound: SoundState;
  toggleSound: () => void;
} {
  const [enabled, setEnabled] = useState(true);
  const [running, setRunning] = useState(false);
  // Initialised to a **constant**, not to audioSupported(): calling it during
  // render returns false on the server (no `window`) and true in the browser,
  // and the first client render must match the server's HTML exactly or React
  // throws a hydration error. Assume support, correct it after mount — that way
  // the common case never flashes, and only a genuinely unsupported browser
  // sees the label change once.
  const [supported, setSupported] = useState(true);
  const seen = useRef<Set<string> | null>(null);

  // Everything that depends on the browser is read after mount, for the same reason.
  useEffect(() => {
    setSupported(audioSupported());
    setEnabled(window.localStorage.getItem(STORAGE_KEY) !== "off");
    setRunning(audioState() === "running");
  }, []);

  useEffect(() => {
    if (!requests) return;

    const nonces = new Set(requests.map((r) => r.nonce));
    if (seen.current === null) {
      seen.current = nonces; // first snapshot: adopt, do not announce
      return;
    }

    const isNew = requests.some((r) => !seen.current!.has(r.nonce));
    seen.current = nonces;
    if (isNew && enabled) void playAlert().then(() => setRunning(audioState() === "running"));
  }, [requests, enabled]);

  // The tab title is the half of the alert that survives a muted browser.
  useEffect(() => {
    const count = requests?.length ?? 0;
    document.title = count > 0 ? `(${count}) ${BASE_TITLE}` : BASE_TITLE;
  }, [requests]);

  const unlock = useCallback(() => {
    void enableAudio().then((ok) => {
      setRunning(ok);
      if (ok) void playAlert(); // play once, so the operator hears what to expect
    });
  }, []);

  const toggleSound = useCallback(() => {
    // "blocked" is not "off": the preference is already on and all that is
    // missing is a user gesture — and this click is one. Unlock instead of
    // switching the preference off, or the button labelled "click to enable"
    // would do the opposite of what it says.
    if (enabled && !running) {
      unlock();
      return;
    }

    const next = !enabled;
    setEnabled(next);
    window.localStorage.setItem(STORAGE_KEY, next ? "on" : "off");
    if (next) unlock();
  }, [enabled, running, unlock]);

  const sound: SoundState = !supported
    ? "unsupported"
    : !enabled
      ? "off"
      : running
        ? "on"
        : "blocked";

  return { sound, toggleSound };
}
