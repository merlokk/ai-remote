/**
 * The subscriber half of `statusline/CLAUDE.md` §9.7, tested against the exact
 * document that file documents.
 *
 * Two things can drift and both are pinned here: the shape of the document (a
 * rename in `statusline/src/status.rs` must fail this suite, not blank a plaque
 * in silence), and the gauge scales §9.2 colours by — the whole point of the
 * plaque is that it reads the same as the line in the terminal.
 *
 * Run: npm test
 */
import assert from "node:assert/strict";
import test from "node:test";

import {
  countdown,
  GAUGE_SCALES,
  gaugeTone,
  isStale,
  relativeAge,
  STALE_AFTER_MS,
  statusDocSchema,
} from "./statusline.ts";

/** Copied from `statusline/CLAUDE.md` §9.7 — what the Rust side actually sends. */
const PUBLISHED = `{
  "ts": 1786136782,
  "line": "Opus 5 (1M context) │ 5h █████░░░ 65% · 1h13m │ 7d ██░░░░░░ 27% · 4d7h │ ctx █░░░░░░░ 12%",
  "session_id": "7b463c0f",
  "cwd": "E:\\\\projects\\\\ai-remote",
  "model": {"id": "claude-opus-5[1m]", "display_name": "Opus 5 (1M context)"},
  "effort": {"level": "high"},
  "rate_limits": {
    "five_hour": {"used_percentage": 65.0, "resets_at": 1786141200,
                  "resets_in": 4418, "resets_in_text": "1h13m"},
    "seven_day": {"used_percentage": 27.0, "resets_at": 1786510800,
                  "resets_in": 374018, "resets_in_text": "4d7h"}
  },
  "context_window": {"used_percentage": 12.0}
}`;

const parse = (text: string) => statusDocSchema.safeParse(JSON.parse(text));

test("reads the document statusline/CLAUDE.md §9.7 documents", () => {
  const parsed = parse(PUBLISHED);
  assert.ok(parsed.success, parsed.error?.message);

  const doc = parsed.data;
  assert.equal(doc.ts, 1786136782);
  assert.equal(doc.model?.display_name, "Opus 5 (1M context)");
  assert.equal(doc.model?.id, "claude-opus-5[1m]");
  // A sibling of `model` on the wire, shown next to it — the same split the Rust
  // side makes: the payload's shape is the contract, the pairing is presentation.
  assert.equal(doc.effort?.level, "high");
  assert.equal(doc.cwd, "E:\\projects\\ai-remote");
  assert.equal(doc.session_id, "7b463c0f");
  assert.equal(doc.context_window?.used_percentage, 12);

  const five = doc.rate_limits?.five_hour;
  assert.equal(five?.used_percentage, 65);
  assert.equal(five?.resets_at, 1786141200);
  assert.equal(five?.resets_in_text, "1h13m");
  assert.equal(doc.rate_limits?.seven_day?.resets_in_text, "4d7h");
});

test("a session with no rate limits still parses", () => {
  // An API key rather than a subscription: §9.7 publishes just `ts` and `line`,
  // and the plaque has to survive that rather than reject the message.
  const parsed = parse(`{"ts": 42, "line": "? │ limits n/a"}`);
  assert.ok(parsed.success);
  assert.equal(parsed.data.rate_limits, undefined);
  assert.equal(parsed.data.context_window, undefined);
  assert.equal(parsed.data.model, undefined);
  assert.equal(parsed.data.effort, undefined);
});

test("an effort without a string level is not this publisher's document", () => {
  // Absent is fine — an older status line simply has no `effort`. Present but
  // malformed is not: our publisher omits the field rather than emitting a broken
  // one, so `{"level": 3}` came from somewhere else, exactly like a `model` with a
  // numeric name. Strict here for the same reason `ts`/`line` are required.
  const effort = (value: string) => parse(`{"ts": 1, "line": "x", "effort": ${value}}`);
  const good = effort(`{"level": "xhigh"}`);
  assert.ok(good.success);
  assert.equal(good.data.effort?.level, "xhigh");
  assert.equal(effort(`{}`).success, false);
  assert.equal(effort(`{"level": 3}`).success, false);
});

test("one window arriving alone leaves the other out", () => {
  const parsed = parse(
    `{"ts": 1, "line": "x", "rate_limits": {"seven_day": {"used_percentage": 24}}}`,
  );
  assert.ok(parsed.success);
  assert.equal(parsed.data.rate_limits?.five_hour, undefined);
  assert.equal(parsed.data.rate_limits?.seven_day?.used_percentage, 24);
  assert.equal(parsed.data.rate_limits?.seven_day?.resets_in_text, undefined);
});

test("junk on the subject is rejected, not shown", () => {
  // `status` is an open subject like `approvals.*`: anyone on the bus can publish
  // to it, and a stray `nats pub status hello` must not replace the readout.
  for (const junk of [
    `"hello"`,
    `[]`,
    `{}`,
    `{"ts": 1}`, // no line
    `{"line": "x"}`, // no ts
    `{"ts": "soon", "line": "x"}`,
    `{"ts": 1, "line": "x", "rate_limits": {"five_hour": {"used_percentage": "65"}}}`,
    `{"ts": 1, "line": "x", "context_window": {}}`,
  ]) {
    assert.equal(parse(junk).success, false, `should have been rejected: ${junk}`);
  }
});

test("percentages are clamped exactly like the bar", () => {
  const parsed = parse(
    `{"ts": 1, "line": "x",
      "rate_limits": {"five_hour": {"used_percentage": 130}},
      "context_window": {"used_percentage": -5}}`,
  );
  assert.ok(parsed.success);
  assert.equal(parsed.data.rate_limits?.five_hour?.used_percentage, 100);
  assert.equal(parsed.data.context_window?.used_percentage, 0);
});

test("the two gauge scales are the ones render.rs uses", () => {
  assert.deepEqual(GAUGE_SCALES.window, { green: 50, yellow: 80 });
  assert.deepEqual(GAUGE_SCALES.context, { green: 20, yellow: 45 });

  assert.equal(gaugeTone("window", 50), "green");
  assert.equal(gaugeTone("window", 50.1), "yellow");
  assert.equal(gaugeTone("window", 80), "yellow");
  assert.equal(gaugeTone("window", 80.1), "red");

  assert.equal(gaugeTone("context", 20), "green");
  assert.equal(gaugeTone("context", 45), "yellow");
  assert.equal(gaugeTone("context", 46), "red");
});

test("the context scale cannot drift into the window one", () => {
  // The mirror of the Rust test: 46% of the context window is red while 46% of a
  // five-hour window is an ordinary Tuesday. If these ever agree, one of them has
  // been "fixed" to match the other.
  assert.equal(gaugeTone("context", 46), "red");
  assert.equal(gaugeTone("window", 46), "green");
});

test("countdown matches render.rs::countdown", () => {
  // Same cases the Rust suite pins, in seconds, so the plaque spells a countdown
  // the way the terminal line does.
  assert.equal(countdown(1000, 1000), "now");
  assert.equal(countdown(1030, 1000), "<1m");
  assert.equal(countdown(1000 + 59 * 60 + 59, 1000), "59m");
  assert.equal(countdown(1000 + 3600 + 59 * 60, 1000), "1h59m");
  assert.equal(countdown(1000 + 3 * 86400 + 5 * 3600, 1000), "3d5h");
  assert.equal(countdown(10, 99_999), "now", "a reset in the past is never negative");
});

test("staleness is measured against the document's own clock", () => {
  // The line publishes on every render, so silence means nobody is working —
  // not that the numbers are still current.
  const ts = 1_786_136_782;
  const nowMs = ts * 1000;
  assert.equal(isStale(ts, nowMs), false);
  assert.equal(isStale(ts, nowMs + STALE_AFTER_MS - 1), false);
  assert.equal(isStale(ts, nowMs + STALE_AFTER_MS + 1), true);
  // A document stamped in the future is a clock that disagrees, not a fresh one.
  assert.equal(isStale(ts, nowMs - 60_000), false);
});

test("relativeAge is coarse, like everything else on the line", () => {
  const ts = 1_786_136_782;
  const at = (secs: number) => relativeAge(ts, (ts + secs) * 1000);
  assert.equal(at(0), "just now");
  assert.equal(at(-5), "just now", "a document from the future is not aged");
  assert.equal(at(4), "4s");
  assert.equal(at(59), "59s");
  assert.equal(at(60), "1m");
  assert.equal(at(3 * 3600 + 20 * 60), "3h");
  assert.equal(at(2 * 86400), "2d");
});
