/**
 * The status line's document, off the `status` subject — `statusline/CLAUDE.md` §9.7.
 *
 * The Rust status line publishes the model and how much of the 5h / 7d rate
 * limits is spent on every render; this module is the subscriber's half of that
 * contract, and it is all this app needs to show the same numbers: the schema for
 * the document, the two gauge scales §9.2 colours by, and the countdown format.
 * The point of the plaque is that it agrees with the line in the terminal, so
 * every rule here is a port of one over there rather than a fresh decision.
 *
 * Read on both sides of the HTTP boundary: the server parses what arrives on the
 * bus, the browser formats it. Which is also why it imports nothing from this app
 * but `zod` — that is what lets `statusline.test.ts` import it directly (see
 * CLAUDE.md, "Conventions": Node's ESM resolver and extensionless imports).
 */
import { z } from "zod";

const clampPercent = (used: number) => Math.min(100, Math.max(0, used));

/**
 * A percentage **spent**, 0–100.
 *
 * The publisher already clamps (§9.7, so the bus and the screen can never
 * disagree) — this clamps again because `status` is an open subject and a bar
 * drawn from someone else's 130% would overflow its track. Same posture as
 * `render.rs::gauge`, which clamps input it also produced.
 */
const percentage = z.number().transform(clampPercent);

/** A rate-limit window. `resets_*` are absent together when the payload had no reset time. */
const windowSchema = z.object({
  used_percentage: percentage,
  /** Unix epoch seconds — the browser recomputes the countdown from this. */
  resets_at: z.number().optional(),
  /** Seconds until `resets_at` at `ts`, for a subscriber that distrusts its own clock. */
  resets_in: z.number().optional(),
  /** The countdown as the line spelled it, e.g. `1h13m`. */
  resets_in_text: z.string().optional(),
});

/**
 * One message on `status`.
 *
 * `ts` and `line` are required because §9.7 always sends them, even for a payload
 * with nothing else in it — a document without them is not the status line's, and
 * on an open subject that is the only cheap way to tell. Everything else is
 * optional exactly where the Rust side omits it: absent is absent, never null.
 */
export const statusDocSchema = z.object({
  ts: z.number(),
  line: z.string(),
  session_id: z.string().optional(),
  cwd: z.string().optional(),
  model: z
    .object({ id: z.string().optional(), display_name: z.string().optional() })
    .optional(),
  rate_limits: z
    .object({ five_hour: windowSchema.optional(), seven_day: windowSchema.optional() })
    .optional(),
  context_window: z.object({ used_percentage: percentage }).optional(),
});

export type StatusDoc = z.infer<typeof statusDocSchema>;
export type StatusWindow = z.infer<typeof windowSchema>;

/** Which scale a gauge is read on — the two are deliberately different. */
export type Gauge = "window" | "context";
export type Tone = "green" | "yellow" | "red";

/**
 * Where a gauge turns yellow and then red, in percent spent — `render.rs`
 * `WINDOW` / `CONTEXT`, §9.2.
 *
 * Half a five-hour window is an ordinary working state; half a context window is
 * most of the way to a compact. A test pins both and asserts they cannot drift
 * into each other, the same way the Rust suite does.
 */
export const GAUGE_SCALES: Record<Gauge, { green: number; yellow: number }> = {
  window: { green: 50, yellow: 80 },
  context: { green: 20, yellow: 45 },
};

export function gaugeTone(gauge: Gauge, usedPercentage: number): Tone {
  const { green, yellow } = GAUGE_SCALES[gauge];
  const used = clampPercent(usedPercentage);
  if (used <= green) return "green";
  if (used <= yellow) return "yellow";
  return "red";
}

/**
 * Time until a window rolls over — a port of `render.rs::countdown`, both
 * arguments in Unix epoch **seconds**.
 *
 * The document carries `resets_in_text` already resolved, and this recomputes it
 * anyway: the plaque ticks once a second off a live clock, so a card that has
 * been on screen for ten minutes must not still claim `1h13m`. The published
 * text stays the fallback for a window that came with no `resets_at`.
 */
export function countdown(resetsAt: number, now: number): string {
  const secs = Math.max(0, Math.trunc(Math.max(0, resetsAt)) - Math.trunc(now));
  if (secs === 0) return "now";
  if (secs < 60) return "<1m";
  if (secs < 3600) return `${Math.floor(secs / 60)}m`;
  if (secs < 86400) return `${Math.floor(secs / 3600)}h${Math.floor((secs % 3600) / 60)}m`;
  return `${Math.floor(secs / 86400)}d${Math.floor((secs % 86400) / 3600)}h`;
}

/**
 * How long a document is worth presenting as current, ms.
 *
 * There is no TTL on the bus for this — it is a current value, superseded on the
 * next render and never persisted (§9.7), so an idle session simply stops
 * publishing. Two minutes of silence is the point where the numbers stop being
 * "now" and become "the last thing we heard", and the plaque says so rather than
 * dropping them: a stale percentage is still the best available answer.
 */
export const STALE_AFTER_MS = 120_000;

/** `ts` is the document's own clock in epoch seconds; `nowMs` is ours, in ms. */
export function isStale(ts: number, nowMs: number): boolean {
  return nowMs - ts * 1000 > STALE_AFTER_MS;
}

/**
 * How old the document is, in one coarse unit — `4s`, `3m`, `2h`, `5d`.
 *
 * Under a second, and anything stamped in the future, reads `just now`: a clock a
 * hair ahead of ours must not produce a negative age in a readout nobody can act
 * on anyway.
 */
export function relativeAge(ts: number, nowMs: number): string {
  const secs = Math.floor((nowMs - ts * 1000) / 1000);
  if (secs < 1) return "just now";
  if (secs < 60) return `${secs}s`;
  if (secs < 3600) return `${Math.floor(secs / 60)}m`;
  if (secs < 86400) return `${Math.floor(secs / 3600)}h`;
  return `${Math.floor(secs / 86400)}d`;
}
