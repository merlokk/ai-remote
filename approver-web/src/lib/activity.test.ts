/**
 * The subscriber half of `statusline/CLAUDE.md` §9.10, tested against the exact
 * documents that file documents.
 *
 * Same job as `statusline.test.ts`: pin the shape, so a rename in
 * `statusline/src/activity.rs` fails this suite instead of quietly blanking a row
 * on the page. Plus the one rule that is this module's own — a document from an
 * open subject is only ours if it says so (`v`).
 *
 * Run: npm test
 */
import assert from "node:assert/strict";
import test from "node:test";

import {
  ACTIVITY_STALE_AFTER_MS,
  activityDocSchema,
  activityHeadline,
  isActivityStale,
  STATE_PALETTE,
} from "./activity.ts";

/** Copied from §9.10 — what the Rust side actually sends before a tool runs. */
const PRE_TOOL = `{
  "v": 1,
  "ts": 1786136782,
  "event": "pre_tool",
  "state": "running",
  "session_id": "7b463c0f",
  "cwd": "E:\\\\projects\\\\ai-remote",
  "tool_name": "Bash",
  "summary": "py -m pytest -q",
  "tool_use_id": "toolu_01ABC123"
}`;

const STOP = `{"v": 1, "ts": 1786136999, "event": "stop", "state": "idle",
               "session_id": "7b463c0f"}`;

const parse = (text: string) => activityDocSchema.safeParse(JSON.parse(text));
const doc = (text: string) => {
  const parsed = parse(text);
  assert.ok(parsed.success, parsed.error?.message);
  return parsed.data;
};

test("reads the document statusline/CLAUDE.md §9.10 documents", () => {
  const pre = doc(PRE_TOOL);
  assert.equal(pre.v, 1);
  assert.equal(pre.ts, 1786136782);
  assert.equal(pre.event, "pre_tool");
  assert.equal(pre.state, "running");
  assert.equal(pre.tool_name, "Bash");
  assert.equal(pre.summary, "py -m pytest -q");
  assert.equal(pre.tool_use_id, "toolu_01ABC123");
  assert.equal(pre.cwd, "E:\\projects\\ai-remote");
  assert.equal(pre.session_id, "7b463c0f");
  assert.equal(pre.agent_type, undefined);
});

test("a turn that ended carries no tool, and none of the model's prose", () => {
  const stop = doc(STOP);
  assert.equal(stop.event, "stop");
  assert.equal(stop.state, "idle");
  assert.equal(stop.tool_name, undefined);
  assert.equal(stop.summary, undefined);
  // The publisher never sends it (§9.10), and the schema would drop it anyway.
  assert.equal("last_assistant_message" in stop, false);
});

test("the minimum document — ts, event, state — is enough", () => {
  const bare = doc(`{"v": 1, "ts": 42, "event": "post_tool", "state": "thinking"}`);
  assert.equal(bare.tool_name, undefined);
  assert.equal(activityHeadline(bare), "thinking", "with nothing to say, say the state");
});

test("only documents that say they are ours are accepted", () => {
  // `activity` is as open as every other subject: a stray publisher must not
  // blank a row that was correct a second ago.
  const cases = [
    `{"ts": 42, "event": "stop", "state": "idle"}`, // no `v` at all
    `{"v": 2, "ts": 42, "event": "stop", "state": "idle"}`, // a version we do not know
    `{"v": 1, "event": "stop", "state": "idle"}`, // no clock
    `{"v": 1, "ts": 42, "event": "compacting", "state": "idle"}`, // an event we do not know
    `{"v": 1, "ts": 42, "event": "stop", "state": "asleep"}`, // a state we do not know
    `{"v": 1, "ts": 42, "event": "stop"}`, // no state
    `"hello"`,
  ];
  for (const text of cases) {
    assert.equal(parse(text).success, false, text);
  }
});

test("the headline reads in the order it is worth reading", () => {
  assert.equal(activityHeadline(doc(PRE_TOOL)), "Bash · py -m pytest -q");

  const subagent = doc(
    `{"v": 1, "ts": 42, "event": "pre_tool", "state": "running",
      "tool_name": "Grep", "summary": "TODO", "agent_type": "Explore"}`,
  );
  assert.equal(activityHeadline(subagent), "Explore › Grep · TODO", "whose work it is comes first");

  const noSummary = doc(
    `{"v": 1, "ts": 42, "event": "pre_tool", "state": "running", "tool_name": "TodoWrite"}`,
  );
  assert.equal(activityHeadline(noSummary), "TodoWrite", "a tool with nothing worth quoting");

  assert.equal(activityHeadline(doc(STOP)), "idle");
});

test("an idle document never goes stale, a busy one does", () => {
  const nowMs = 1786136782_000 + ACTIVITY_STALE_AFTER_MS + 1000;

  // "running Bash" stops being believable after ten minutes of silence.
  assert.equal(isActivityStale(doc(PRE_TOOL), nowMs), true);
  assert.equal(isActivityStale(doc(PRE_TOOL), 1786136782_000 + 1000), false);

  // "idle" stays true until something else happens — greying it out would
  // suggest the session vanished when it is simply done.
  assert.equal(isActivityStale(doc(STOP), nowMs + 86_400_000), false);
});

test("nothing on this row is ever red", () => {
  // A busy session is not a problem, and red on this page belongs to a denial.
  assert.equal(Object.values(STATE_PALETTE).includes("red"), false);
  assert.equal(STATE_PALETTE.running, "brand");
  assert.equal(STATE_PALETTE.idle, "gray");
});
