/**
 * Parity tests against `approver/protocol.py`.
 *
 * The expected values were produced by the Python implementation itself — these
 * are cross-language vectors, not self-consistency checks. If this file goes red
 * after a change here, `hook.py` will reject every reply this app signs.
 *
 * Run: npm test   (node:test + native TS type stripping, no test runner dep)
 */
import assert from "node:assert/strict";
import test from "node:test";

import { canonicalJson, canonicalSha256, signingBytes } from "./protocol.ts";

const VECTORS = [
  {
    obj: { b: 1, a: 2 },
    canon: '{"a":2,"b":1}',
    sha: "d3626ac30a87e6f7a6428233b3c68299976865fa5508e4267c5415c76af7a772",
  },
  {
    obj: { command: "rm -rf build", z: [1, 2, { k: "v" }] },
    canon: '{"command":"rm -rf build","z":[1,2,{"k":"v"}]}',
    sha: "7e84def982a281e081a9044d4e5625b3c8a1adb54ccc444733be539f1903183b",
  },
  {
    // The one that bites: Python's ensure_ascii=True escapes these, JSON.stringify does not.
    obj: { text: "привет — ok", emoji: "x" },
    canon: '{"emoji":"x","text":"\\u043f\\u0440\\u0438\\u0432\\u0435\\u0442 \\u2014 ok"}',
    sha: "6cd1719de7a27c8fb446946b161b2b16109dd519dbd0e91454f3432fa4a093b0",
  },
  {
    obj: {},
    canon: "{}",
    sha: "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a",
  },
];

test("canonicalJson matches Python json.dumps(sort_keys=True, separators=(',',':'))", () => {
  for (const v of VECTORS) assert.equal(canonicalJson(v.obj), v.canon);
});

test("canonicalSha256 matches approver.protocol.canonical_sha256", async () => {
  for (const v of VECTORS) assert.equal(await canonicalSha256(v.obj), v.sha);
});

test("signingBytes matches approver.protocol.signing_bytes", () => {
  const bytes = signingBytes({
    v: 1,
    session_id: "abc123",
    nonce: "bm9uY2U=",
    tool_name: "Bash",
    input_sha256: "a".repeat(64),
    behavior: "allow",
    updated_input_sha256: "",
    ts: 1737345600,
    reason: "looks fine\nreally",
  });
  assert.equal(
    new TextDecoder().decode(bytes),
    "1\nabc123\nbm9uY2U=\nBash\n" +
      "a".repeat(64) +
      "\nallow\n\n1737345600\nlooks fine\nreally",
  );
});

test("a multiline reason stays unambiguous as the tail field", () => {
  // reason is last precisely so its newlines cannot be confused with separators.
  const decode = (b: Uint8Array) => new TextDecoder().decode(b);
  const base = {
    v: 1,
    session_id: "s",
    nonce: "n",
    tool_name: "Bash",
    input_sha256: "h",
    behavior: "deny",
    updated_input_sha256: "",
    ts: 1,
  };
  assert.notEqual(
    decode(signingBytes({ ...base, reason: "a\nb" })),
    decode(signingBytes({ ...base, reason: "a\nc" })),
  );
});
