"use client";

/**
 * The browser half of the transport: one EventSource, one snapshot in state.
 *
 * EventSource reconnects by itself, and every frame is a full snapshot, so a
 * dropped connection needs no resync logic here.
 */
import { useEffect, useState } from "react";

import type { Snapshot } from "./types";

export interface StreamState {
  snapshot: Snapshot | null;
  /** The SSE channel to our own server — not the NATS connection (that is in `snapshot.status`). */
  streamOpen: boolean;
}

export function useApprovalStream(): StreamState {
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [streamOpen, setStreamOpen] = useState(false);

  useEffect(() => {
    const source = new EventSource("/api/stream");

    source.onopen = () => setStreamOpen(true);
    source.onmessage = (event) => {
      try {
        setSnapshot(JSON.parse(event.data) as Snapshot);
        setStreamOpen(true);
      } catch {
        // A truncated frame is not worth tearing the page down over.
      }
    };
    source.onerror = () => setStreamOpen(false);

    return () => source.close();
  }, []);

  return { snapshot, streamOpen };
}

/**
 * A clock that ticks, for the countdown on each card.
 *
 * `Date.now()` as the initial state would normally be a hydration hazard — the
 * server's value can never match the client's. It is safe only because nothing
 * renders it server-side: cards exist once the SSE snapshot arrives, and that is
 * client-only. Keep it that way; if a request ever renders during SSR, this has
 * to move into an effect.
 */
export function useNow(intervalMs = 1000): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), intervalMs);
    return () => clearInterval(id);
  }, [intervalMs]);
  return now;
}
