# approver-web — the responder as a web page

A third responder for the approval flow described in the repository-root
`CLAUDE.md` §6/§7, alongside `approver/responder.py` (software key) and
`approver/responder_yubikey.py` (key on a YubiKey, the primary one). Same
subjects, same `handler-config.json` allowlist, same signing bytes — the hook,
the protocol module and the registration handler know nothing about this app and
did not change for it.

The difference is only the front end: instead of a console prompt, the operator
gets a page that lists pending requests and answers them with a button.

## Status: phase 1 of 2

| Phase | Scope | State |
|-------|-------|-------|
| **1** | See requests off the bus, answer allow/deny, reply into the request's inbox — **no signature** | **done, this scaffold** |
| **2** | Sign the decision with a registered key, plus a registration page for the one-time token | planned, see "Phase 2" below |

**What phase 1 does *not* do: make a decision take effect.** The reply carries
`sig: ""`, `hook.py` rejects it as untrusted and Claude Code falls back to its
own prompt (§7). That is deliberate — the round trip is fully observable while no
key exists, and there is no code path by which a missing signature turns into an
allow. The UI says so in the status bar, and `POST /api/decision` reports
`signed: false`.

## Architecture

```mermaid
sequenceDiagram
    autonumber
    participant CC as Claude Code
    participant H as hook.py
    participant N as NATS
    participant S as Next.js server<br/>(lib/responder.ts)
    participant B as Browser<br/>(app/page.tsx)
    participant O as Operator

    CC->>H: PermissionRequest on stdin
    H->>N: request approvals.<session_id>
    N->>S: delivery (queue group "approvers")
    S->>S: zod-validate, hold the msg, start the TTL
    S-->>B: SSE snapshot {status, requests}
    B->>O: card: tool, cwd, tool_input
    O->>B: Allow / Deny (+ reason, + replacement input)
    B->>S: POST /api/decision {nonce, behavior, reason}
    S->>S: signingBytes() -> signer.sign()  (phase 1: "")
    S-->>N: msg.respond(reply)
    N-->>H: reply inbox
    H->>H: verify sig against clients[key_id]
    H-->>CC: allow/deny (exit 0), or fall back to the prompt
```

### Why the *server* holds the NATS connection

A browser cannot open a TCP socket to `:4222`. There were two ways out and this
app takes the second:

1. **`nats.ws` in the browser.** Requires a `websocket { … }` listener on the
   NATS server, i.e. editing `nats/docker-compose.yml` and opening another port.
   The page then *is* a NATS client — appealing, and worth revisiting if this ever
   has to run on a machine that is not the one the hook runs on.
2. **The Next.js server is the NATS client; the browser drives it over
   SSE + POST.** ← chosen.

Reasons for (2), in order of weight:

- **No infrastructure change.** The existing compose file and the Python
  responders keep working untouched; this app is additive.
- **The reply inbox has to be held somewhere.** A `Msg` is answerable only by the
  process that received it. Server-side, a page reload costs nothing; with
  `nats.ws` a reload drops every in-flight request on the floor.
- **`config.json` is read with `fs`,** exactly like the Python responders read
  theirs, and phase 2's key material never has to be shipped to a browser.
- **Phase 2 keeps both options.** The signing seam is a `sign(bytes) -> b64`
  callable (below); it can live on the server *or* in the browser (WebCrypto)
  without touching the transport.

Cost of (2), stated plainly: the machine running the Next server is a trusted
component. It sees every `tool_input` and it is what actually answers the bus.
That is the same trust level as the Python responders, which run on that machine
too — but it is *not* the same as "the browser is the responder", so a remote
operator on a phone is trusting the host in a way they would not with `nats.ws`.

### Modules

```
config.json              runtime config (git-ignored; config.example.json is the reference)
src/lib/
  protocol.ts            port of approver/protocol.py — canonical JSON, sha256, signing bytes
  protocol.test.ts       cross-language parity vectors (node:test)
  schemas.ts             every zod schema: config, bus request, decision form, decision POST
  config.ts              config.json loader (server)
  signer.ts              the sign(bytes) -> b64 seam. Phase 1 = unsignedSigner
  reply.ts               port of responder.build_signed_reply
  responder.ts           server-only: NATS connection, pending map, TTL sweep, decide()
  types.ts               types shared with the browser (kept out of responder.ts on purpose)
  use-approval-stream.ts client: EventSource -> snapshot, plus a ticking clock
src/app/
  layout.tsx providers.tsx page.tsx      the single page
  api/stream/route.ts    GET  — SSE, full snapshot per frame; opening it starts NATS
  api/decision/route.ts  POST — validate, sign, respond into the reply inbox
src/components/
  StatusBar.tsx RequestCard.tsx DecisionForm.tsx
```

Mirrored from the Python side on purpose: `protocol.ts` ↔ `protocol.py`,
`reply.ts` ↔ `responder.build_signed_reply`, `signer.ts` ↔ the `sign=` parameter
that lets `responder_yubikey.py` reuse all of `responder.py`. Keeping those seams
in the same places is what makes "does the web responder still match?" a question
you can answer by reading two short files.

### Transport contract (browser ↔ our own server)

| Route | Shape |
|-------|-------|
| `GET /api/stream` | `text/event-stream`. Every frame is a **whole** `{status, requests}` snapshot, so a reconnect needs no resync. `: ping` every 15 s. Opening the stream is what triggers the NATS connect — there is no connect button, because having the page open *is* being a responder. |
| `POST /api/decision` | `{nonce, behavior, reason, updated_input?}` → `{ok, signed}`. `409` = the request is gone (expired or already answered). `500` = signing failed — nothing was sent, so the hook falls back. |

`nonce` is the entry id: it is unique per request and already on the wire.

### config.json

```json
{ "v": 1, "servers": "nats://127.0.0.1:4222", "subject": "approvals.*",
  "queue": "approvers", "key_id": "approver-web", "key_type": "p256",
  "request_ttl": 120 }
```

Every field has a default, so the app runs with no config file at all (the status
bar says so). This is looser than `lib/config.py`, which fails fast when a
responder has no key — phase 1 has no key to miss. **Phase 2 changes that:** once
a key is required, a missing/unregistered one must refuse to start, the way
`responder_yubikey.py serve` refuses when its re-derivation does not match the
registered public key.

`request_ttl` should be ≥ the hook's own `timeout` in `handler-config.json`,
otherwise a card disappears while Claude Code is still waiting.

## Rules this app inherits (do not soften them)

- **No reply is the safe outcome.** Expiry, a signing failure, a closed tab —
  all end in silence, the hook times out, Claude Code shows its own prompt.
  Never invent a verdict, and never answer with a guess. There is no "skip"
  button because skipping is exactly *not pressing anything*.
- **Untrusted input on an open subject.** `approvals.*` is public to anyone on
  the bus. A non-JSON payload, a malformed request, a message with no reply
  inbox: dropped with one warning line, never crashes the subscription loop.
  Same rule as `lib/bus.py`.
- **The signature covers `updated_input`, which never travels as a hash.** The
  hook recomputes `sha256(canonical(updated_input))` from the object it received;
  if the responder signed something else, the signature simply fails.
- **`key_type` is pinned by the allowlist**, not by the reply. Nothing this app
  sends can select the verification algorithm.

### Canonical JSON: the one real interop trap

`approver/protocol.py` builds hashes with
`json.dumps(obj, sort_keys=True, separators=(",", ":"))` — and `ensure_ascii`
defaults to **True**, so Python escapes every non-ASCII character as `\uXXXX`
(lowercase). `JSON.stringify` emits it literally. Without the re-escape pass in
`canonicalJson`, any `updated_input` containing a non-ASCII character produces a
different hash on each side and every such decision is rejected with no
explanation beyond "signature verification failed".

`protocol.test.ts` pins this with vectors generated by the Python implementation
itself. Regenerate them with:

```powershell
py -c "import sys; sys.path.insert(0, r'E:\projects\ai-remote'); from approver import protocol as p; print(p.canonical_json({'text':'привет'}), p.canonical_sha256({'text':'привет'}))"
```

Known remaining gap: floats. Python's `repr` and JS number formatting agree for
everything realistic in a `tool_input`, but they are not the same algorithm. If a
tool ever ships a float in its input, add a vector.

## Stack

Everything here was named in the request, plus the two things it implies:

| Dependency | Why |
|------------|-----|
| `next`, `react`, `react-dom` | app router, server routes for the NATS half |
| `@chakra-ui/react` + `@emotion/react` | UI (emotion is Chakra's required peer) |
| `react-hook-form` + `@hookform/resolvers` + `zod` | the decision form, and zod also validates config + everything off the bus |
| `@nats-io/transport-node` | the NATS client. **Not `nats`** — that package is published-deprecated in favour of this one |
| dev: `typescript`, `@types/*` | — |

**No test-runner dependency.** Tests are `node:test` + Node's native TypeScript
type stripping: `npm test`. The repo's TDD rule (root §1) applies here as it does
to pytest — behavior changes come with tests.

Conventions worth knowing before editing:

- Imports are **extensionless** everywhere except `protocol.test.ts`, which
  imports `./protocol.ts` with the extension because Node's ESM resolver
  requires it. `allowImportingTsExtensions` is on for that one file. If a future
  test needs a module that imports others, either add the extensions along that
  chain or give in and add a runner.
- `serverExternalPackages: ["@nats-io/transport-node"]` in `next.config.ts`
  keeps the Node client out of the bundler.
- `loadConfig()` reads a runtime path, so the `readFileSync` call carries a
  `/* turbopackIgnore: true */` comment — without it the build traces the whole
  project into the server output.
- The responder is a single instance parked on `globalThis`, so dev-server hot
  reload does not leave a second subscriber in the queue group.
- `agentRules: false` in `next.config.ts`. Without it `next dev` appends a
  generated block to this file on every run. Its actual point is worth keeping,
  so: **Next 16 is not the Next.js most training data describes** — app router,
  Turbopack by default, `serverExternalPackages`, async request APIs. The version
  in use ships its own docs in `node_modules/next/dist/docs/`; read those rather
  than recalling Next 13/14 conventions.

## Run — `run.cmd`

The front door. It resolves its own directory first, so it works from anywhere:

```bat
REM plain console, no elevation:
approver-web\run.cmd                REM dev server on http://localhost:3000/

REM or from inside approver-web\:
run.cmd --port 3100                 REM a different port
run.cmd --prod                      REM npm run build, then serve that build
run.cmd --install                   REM force npm install (after editing package.json)
run.cmd --help
```

A normal start looks like this — three numbered steps, then Next takes over:

```
[1/3] dependencies ok
[2/3] NATS port 4222 is listening
[3/3] starting the dev server on http://localhost:3100/ - Ctrl+C to stop

> approver-web@0.1.0 dev
> next dev --port 3100

  Next.js 16.3.0 (Turbopack)
  - Local:   http://localhost:3100
```

- Step 1 runs `npm install` when `node_modules` is missing (first clone) or when
  `--install` says so.
- Step 2 is a **warning only**: with nothing listening on 4222 it prints how to
  start NATS and carries on. The app runs fine without a bus and reports the
  broken link in its own status bar — better than a script refusing to start.
- Exit codes: `0` the server exited cleanly (Ctrl+C), `1` it could not start —
  no node/npm on PATH, `npm install` failed, or `--prod` failed to build. Each
  prints which one.

Unlike everything in `scripts\`, this needs **no elevation and no venv**: it is
the Node half of the repo and touches no FIDO device. The file is CRLF and opens
no parenthesised `if` blocks, for the reason `scripts\yubikey-approval.cmd`
documents at length.

## Testing

```powershell
npm test        # protocol parity vs. the Python vectors — no NATS, no browser
npm run build   # type-checks everything, including the tests
```

A live check without Claude Code: bring NATS up (`cd nats && docker compose up
-d`), start the app, then from the repo root send a §7 request with
`lib/bus.py` and answer it with a `POST /api/decision`. The reply comes back on
the requester's inbox and `hook.verify_reply` rejects it for exactly one reason —
`signature verification failed` — with every echoed field already correct. That
is the phase-1 finish line, and it has been run.

**A running instance joins the `approvers` queue group and competes with the
Python responders for real traffic.** This was observed during that check: the
app started catching live `PermissionRequest`s from the Claude Code session in
this very repository. Run **one** responder at a time. It is also a fine way to
watch what the hook sends — just be aware that whatever it catches, the YubiKey
responder does not see.

## Phase 2 — signing

Goal: `hook.py` accepts this app's decisions. Nothing about the transport, the
UI or the wire format changes; the work is a key, a registration, and one
implementation of `Signer`.

### Where the key should live

| Option | Key custody | Verdict |
|--------|-------------|---------|
| **Browser, WebCrypto P-256, non-extractable, IndexedDB** | never on disk in usable form, never leaves the tab | **recommended.** Closest in spirit to the YubiKey responder; the server becomes a relay that cannot forge a decision |
| Server, software key in `config.json` | a private key on disk | simplest, and the exact equivalent of `responder.py`. Fine as an interim step, but it is the weaker story |
| Browser + WebAuthn / passkey (touch per decision) | in an authenticator | **does not work as-is.** WebAuthn signs `authenticatorData ‖ sha256(clientDataJSON)`, not our signing bytes, so `hook.py` rejects it. Supporting it means teaching the *hook* a second verification shape — a protocol change (§7), not a front-end change |
| Browser + YubiKey ARKG (`previewSign`) | on the device | not reachable: `previewSign` is a CTAP2 extension, and the browser WebAuthn API does not expose it. That path stays with `responder_yubikey.py` |

Both real options plug into the same seam:
`signer.ts` → `{keyId, keyType, mode, sign(bytes) -> base64}`. For the browser
variant the server's signer becomes "ask the connected tab", i.e. the POST body
carries `sig` and the server only checks shape before responding.

### Two encodings that will otherwise waste an afternoon

`lib/crypto.py` `key_type="p256"` expects, and the hook verifies against:

- **public key** — base64 of the **33-byte compressed SEC1 point**
  (`0x02|0x03` by y-parity, then x). WebCrypto exports `raw` as the **65-byte
  uncompressed** point and `spki` as DER. Compress before registering — the
  YubiKey responder needed the same conversion, `lib/yubikey.p256_public_b64`.
- **signature** — **DER** (`SEQUENCE { r, s }`, variable length). WebCrypto
  `ECDSA` produces **raw `r ‖ s`**, 64 bytes fixed. Convert before sending.

And hash exactly once: `crypto.verify(..., "p256")` runs `ECDSA(SHA256)` over the
signing bytes, so the signer must hash them once and no more.

### Steps

1. **Registration UI.** A second page (or a panel) that takes a one-time token
   `<key_id>.<secret>` minted by
   `py approver/registration_handler.py --get-token approver-web`, generates the
   key pair, and sends the §6 `registrations` request with
   `key_type: "p256"` and the compressed public key. React Hook Form + Zod
   again — the token format (`key_id` non-empty, no `.` in it) is a real
   validation, not decoration. Persist **only on `ok: true`**, like both Python
   responders: a rejected registration must not clobber a working key.
2. **`Signer` implementation** + `signing_mode` in the status bar going green.
3. **Refuse to serve without a usable key**, mirroring
   `responder_yubikey.py serve`: if the stored public key is not what
   `clients[key_id]` holds, say so loudly instead of signing replies the hook
   will silently reject.
4. **Tests.** Sign in the app, verify with `lib/crypto.verify` from Python (a
   fixture file is enough), and a `hook.verify_reply` round trip — the mirror of
   the YubiKey integration test that proves a real device signature is accepted.
5. **A `.cmd` script** in `scripts/`, in the style of `e2e-approval.cmd`, once
   there is something end-to-end worth scripting.

### Not in scope, but worth deciding before shipping to anyone else

The page has **no authentication**. Anyone who can reach `localhost:3000` can
approve a `rm -rf`, and once phase 2 lands, sign it with a trusted key. That is
acceptable for a local single-user tool and unacceptable the moment it binds to
anything but the loopback interface. Root §7 already says the bus itself must not
be exposed; this page is one more thing on that list.
