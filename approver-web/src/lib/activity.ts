/**
 * What Claude is doing, off the `activity` subject — `statusline/CLAUDE.md` §9.10.
 *
 * The sibling of `statusline.ts`. That module reads the document that says what a
 * session is *spending*; this one reads the document that says what it is *doing*,
 * published by the same binary from Claude Code's `PreToolUse` / `PostToolUse` /
 * `Stop` hooks. Read-only, like the plaque: nothing here answers, nothing here
 * touches the approval flow.
 *
 * Imports nothing from this app but `zod`, for the same reason `statusline.ts`
 * does not — that is what lets `activity.test.ts` import it directly under
 * Node's ESM resolver (CLAUDE.md, "Conventions").
 */
import { z } from "zod";

/** The three events §9.10 publishes, and the three states they imply. */
export const ACTIVITY_EVENTS = ["pre_tool", "post_tool", "stop"] as const;
export const ACTIVITY_STATES = ["running", "thinking", "idle"] as const;

export type ActivityEvent = (typeof ACTIVITY_EVENTS)[number];
export type ActivityState = (typeof ACTIVITY_STATES)[number];

/** The schema version the Rust side stamps (`activity::SCHEMA_VERSION`). */
export const ACTIVITY_VERSION = 1;

/**
 * One message on `activity`.
 *
 * `v` is required and pinned: `activity` is an open subject like every other, and
 * unlike the status document — which is recognisable by always carrying `ts` and
 * `line` — everything here except `ts`, `event` and `state` may be absent. So `v`
 * is what tells a document of ours from a stray `nats pub`, and a future `v: 2`
 * is rejected rather than half-understood.
 *
 * `event` and `state` are enums on purpose: the publisher owns both sets, and a
 * value outside them means this page is older than the publisher and should say
 * nothing rather than render a word it cannot colour.
 */
export const activityDocSchema = z.object({
  v: z.literal(ACTIVITY_VERSION),
  /** Unix epoch seconds — when the hook fired. There is nothing to count down. */
  ts: z.number(),
  event: z.enum(ACTIVITY_EVENTS),
  state: z.enum(ACTIVITY_STATES),
  session_id: z.string().optional(),
  cwd: z.string().optional(),
  /** Absent on `stop`, which is not about a tool. */
  tool_name: z.string().optional(),
  /** One value out of the tool's input, flattened and cut to 80 chars (§9.10). */
  summary: z.string().optional(),
  /** Claude Code's id for the tool call, so a `post_tool` matches its `pre_tool`. */
  tool_use_id: z.string().optional(),
  /** Present when the work is inside a subagent — `Explore`, `Plan`, … */
  agent_type: z.string().optional(),
});

export type ActivityDoc = z.infer<typeof activityDocSchema>;

/** What each state is called on screen, and how loudly. */
export const STATE_LABELS: Record<ActivityState, string> = {
  running: "running",
  thinking: "thinking",
  idle: "idle",
};

/**
 * `brand` for work in progress, grey for a finished turn — the same restraint the
 * plaque follows: this is a readout, and the buttons on the request cards must stay
 * the heaviest thing on the page (CLAUDE.md, "Look and feel", constraint 1). No
 * red anywhere: nothing here is a problem, it is just a session being busy or not.
 */
export const STATE_PALETTE: Record<ActivityState, string> = {
  running: "brand",
  thinking: "brand",
  idle: "gray",
};

/**
 * The one line to show: what is happening, in the order it is read.
 *
 * `Bash · py -m pytest -q`, `Explore › Grep · TODO`, or just `idle` for a turn
 * that has ended. The tool name comes first because it is the part that is always
 * there; the summary is what makes it worth reading.
 */
export function activityHeadline(doc: ActivityDoc): string {
  const parts = [doc.tool_name, doc.summary].filter(Boolean);
  if (parts.length === 0) return STATE_LABELS[doc.state];

  const what = parts.join(" · ");
  return doc.agent_type ? `${doc.agent_type} › ${what}` : what;
}

/**
 * How long a document is worth presenting as *current*, ms.
 *
 * Longer than the plaque's two minutes (`statusline.ts`, `STALE_AFTER_MS`) and for
 * a different reason. The status line republishes on every render, so silence there
 * means something stopped; hooks fire only when a tool runs, so a session thinking
 * hard — or waiting on a permission request — legitimately says nothing for a
 * while. Ten minutes is the point where "running Bash" stops being believable.
 *
 * A `stop` document never goes stale: "idle" stays true until something else
 * happens, and greying it out would suggest the session vanished when it is simply
 * done. See `isActivityStale`.
 */
export const ACTIVITY_STALE_AFTER_MS = 600_000;

/** `ts` is the document's clock in epoch seconds; `nowMs` is ours, in ms. */
export function isActivityStale(doc: ActivityDoc, nowMs: number): boolean {
  if (doc.state === "idle") return false;
  return nowMs - doc.ts * 1000 > ACTIVITY_STALE_AFTER_MS;
}
