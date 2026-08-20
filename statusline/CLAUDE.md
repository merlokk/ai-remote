# The status line

## 9. `statusline/` — model and limits spent, in Rust

The bar Claude Code draws at the bottom of the terminal. It answers the two
questions worth a permanent row of screen: **which model is answering** and
**how much of the rate limits is already spent**.

It replaced a Python one-liner that printed a banner. Rust, because the status
line is re-rendered on every turn: a compiled binary starts in single-digit
milliseconds where `py script.py` pays interpreter startup each time.

**The same binary has a second job** (§9.10): wired as Claude Code's
`PreToolUse` / `PostToolUse` / `Stop` hook, it publishes what the session is
*doing* — the tool about to run, the tool that just ran, the turn that ended — to
a subject beside the one the numbers go to. Same reason for Rust, only more so: a
hook fires twice per tool call, not once per turn.

### 9.1 The contract

Claude Code pipes one JSON object in on stdin and prints whatever comes back on
stdout ([docs](https://code.claude.com/docs/en/statusline)). The fields this
binary reads — everything else in the payload is ignored:

| Field | Used for |
|-------|----------|
| `model.display_name` | the name at the head of the line |
| `effort.level` | the reasoning effort, hung off that name (`· high`) |
| `rate_limits.five_hour.used_percentage` / `.resets_at` | the `5h` window: bar, percent spent, countdown |
| `rate_limits.seven_day.used_percentage` / `.resets_at` | the `7d` window, same shape |
| `context_window.used_percentage` | the trailing `ctx` gauge |
| `model.id`, `session_id`, `cwd` | nothing on the line — they go only into the published document (§9.7) |

`used_percentage` is 0–100 and counts what is **spent**; the line prints it as
it comes, so every number on the row means the same thing — a bar that grows
towards the limit. `resets_at` is Unix epoch seconds, rendered as a countdown
(`2h14m`, `4d8h`, `<1m`, `now`) rather than a wall-clock time — no timezone
handling, and no timezone crate.

**`rate_limits` is not always there.** It appears only for Claude.ai
subscribers, and only after the session's first API response; each window may be
absent on its own. Missing both prints `limits n/a`.

### 9.2 The layout

```
● Opus 5 (1M context) · high │ 5h ████░░░░ 44% · 2h14m │ 7d ██░░░░░░ 24% · 4d8h │ ctx ░░░░░░░░ 6%
```

`· high` is `effort.level`, and it is deliberately **not** a `│`-separated field
of its own: it is what that model is currently doing, so it hangs off the name and
is muted, leaving the name as the thing the eye lands on. A payload without it —
an older Claude Code — prints the name alone, with no dot and no trailing gap.

The leading dot is the bus: **green — connected, red — not** (§9.8). It is not
a `│`-separated segment on purpose — it reads as a lamp on the line rather than
a fourth field competing with the model name — and it disappears entirely when
publishing is switched off, because then there is no link for it to report.

**Every number on the row is the same kind of gauge** — a label, an 8-cell bar,
a percentage — so the line is read with one habit instead of three. Bars fill
with what is **spent**: an empty bar is a fresh window, a full one is one you
have used up. `ctx` is a gauge like the rest; it just has no countdown, because
a context window does not reset on a clock, it resets when the session does.

The colour is a traffic light on that same number, and the **scale differs per
gauge** because the same percentage does not mean the same thing:

| Gauge | Green | Yellow | Red |
|-------|-------|--------|-----|
| `5h` / `7d` rate limits (`WINDOW`) | ≤50% spent | ≤80% | above |
| `ctx` context window (`CONTEXT`) | ≤20% spent | ≤45% | above |

Half a five-hour window is an ordinary working state; half a context window is
most of the way to a compact, and the useful moment to notice is well before it
is full. A test pins both scales and asserts they cannot drift into each other.
The dot is the only other thing on the line that can be red, and only when the
bus is down.

The palette assumes a **black terminal background**. Text is an explicit bright
grey (`\x1b[38;5;252m`) and labels, separators and countdowns a step darker
(`247`) — neither the terminal's default foreground nor `\x1b[2m` dimming is
used, because on black the first is unpredictable and the second is unreadable.
On a light background, lower both numbers.

### 9.3 Failure is silent, always

A status line that panics replaces the status bar with a stack trace, so
`main()` has no `unwrap` on anything derived from input: unreadable stdin ends
up an empty string, unparseable JSON becomes `Json::Null`, and every field read
is an `Option` that degrades to a placeholder (`?` for the model, `limits n/a`
for the windows, no segment at all for a missing context window). Percentages
outside 0–100 are clamped and a `resets_at` in the past reads `now` instead of
underflowing.

The bus obeys the same rule and then some: everything in §9.7–§9.8 happens
*after* the line is printed, returns a `Result` that `main` discards, and can be
switched off entirely with `"publish": false` (§9.9). A malformed config, an
unreachable server, an unwritable temp directory or a runtime that will not start
all cost at most a red dot.

### 9.4 Layout of the crate

A library plus a binary. `src/main.rs` is the Claude Code side and does nothing
`src/lib.rs` does not expose, so anything else in the repo that wants these
numbers links the library instead of parsing a terminal line back out of a pipe.

- `src/main.rs` — the binary, and the fork between its two jobs (§9.10): config
  → stdin → parse → either `render` → stdout → publish (§9.7), or publish an
  activity document and print nothing.
- `src/lib.rs` — the library root; the six modules below.
- `src/json.rs` — the payload, read with `serde_json`. `Value` plus a `Lookup`
  trait adding `path("a.b.c")` / `str_at` / `num_at`: dotted paths because that
  is how §9.1 and the Claude Code reference name the fields, where `pointer()`
  would want `/a/b`. Object keys only — the payload holds no arrays we read.
- `src/render.rs` — pure formatting.
  `render(&Json, now: u64, link: Link) -> String` takes both the clock and the
  previous render's bus verdict as arguments, so the countdowns and the dot are
  testable without a clock or a server. `strip_ansi` is the colourless version,
  for the bus and for the tests.
- `src/status.rs` — the published document as `serde` structs (§9.7).
- `src/activity.rs` — the *other* published document, and the dispatch that
  recognises a hook payload (§9.10).
- `src/nats.rs` — the NATS client wrapper (§9.7).
- `src/link.rs` — the cached reachability verdict behind the dot (§9.8).
- `src/config.rs` — `statusline-config.json` (§9.9).

**Dependencies.** This crate started with none, on the theory that a binary
running on every render should carry no supply chain. Publishing to NATS ended
that: `async-nats` is the official client and reimplementing the protocol to
avoid it would be far worse than depending on it.

| Crate | Why |
|-------|-----|
| `async-nats` | the NATS client (root `CLAUDE.md` §1 lists `nats-py` for the Python side; this is its Rust counterpart) |
| `tokio` (`rt`, `time`, `macros`) | `async-nats` is async and brings no runtime of its own; §9.7 builds a current-thread one per render |
| `serde` (`derive`) + `serde_json` | the wire format both ways — already in `async-nats`'s tree, so they cost nothing extra |
| `futures` (**dev only**) | `Subscriber` is a `Stream`, and reading one in the integration test needs `StreamExt`. Dev-dependency: not linked into the binary, and likewise already in the tree |

`cargo build` therefore needs the network on a cold registry, and `uv sync` is
still untouched — none of this reaches the Python side.

### 9.5 Build and wiring

```
cd statusline
cargo test           # unit tests, plus the NATS round trip if a server is up
cargo build --release
```

The binary lands at `statusline/target/release/statusline.exe` (git-ignored),
and `.claude/settings.json` points the status line at it:

```json
{
  "statusLine": {
    "type": "command",
    "command": "\"E:\\projects\\ai-remote\\statusline\\target\\release\\statusline.exe\"",
    "padding": 0
  }
}
```

**The inner quotes are load-bearing.** Claude Code runs the command through a
shell, and in `sh` a backslash escapes the next character — an unquoted Windows
path arrives as `E:projectsai-remotestatuslinetargetreleasestatusline.exe` and
the status line silently stays empty (`command not found` goes nowhere the UI
shows). Quoting the path inside the JSON string keeps the backslashes literal;
the same trick is why the `PermissionRequest` hook in `settings.local.json` is
written the way it is.

The same file wires the same binary as three hooks (§9.10) — the quoting is
load-bearing there for the same reason:

```json
{
  "hooks": {
    "PreToolUse": [{ "matcher": "*", "hooks": [{ "type": "command",
      "command": "\"E:\\projects\\ai-remote\\statusline\\target\\release\\statusline.exe\"",
      "timeout": 5 }] }],
    "PostToolUse": [{ "matcher": "*", "hooks": [{ "…": "the same" }] }],
    "Stop":        [{ "hooks": [{ "…": "the same, and no matcher — Stop has nothing to match" }] }]
  }
}
```

`timeout` is seconds and is belt-and-braces: the binary's own budget is 500 ms
(§9.7) and §9.8 means it usually opens no socket at all. A hook added while a
session is running is picked up by that session's settings watcher; if the row
never appears, open `/hooks` once or restart.

Project settings win over the user-level `~/.claude/settings.json`, so this
binary is the status line inside this repo only; other projects keep whatever is
configured globally.

To see the line without Claude Code, feed it a payload:

```
type statusline\payload.example.json | statusline\target\release\statusline.exe
```

`payload.example.json` is a real payload captured from Claude Code, with the
session identifiers scrubbed — the reference for what the fields actually look
like, next to the docs link above.

To change what it publishes, copy `statusline-config.example.json` from the
repository root next to the binary and edit it (§9.9):

```
copy statusline-config.example.json statusline\target\release\statusline-config.json
```

**Toolchain note (Windows).** The default Rust target is
`x86_64-pc-windows-msvc`, which links with MSVC's `link.exe` — it comes from the
Visual Studio "Desktop development with C++" workload, not from the Rust
installer, and a machine without it fails at the link step with
`linker 'link.exe' not found`. Install it with
`winget install --id Microsoft.VisualStudio.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`.
Beware of building from Git Bash before that: Git for Windows ships its own
unrelated `link.exe`, and rustc picks it up and fails with a confusing
`missing operand` from GNU coreutils.

### 9.6 Tests

`cargo test` — the unit tests live next to the code in `#[cfg(test)] mod tests`,
which is where Rust puts them (the repo's `tests/` directory is pytest's, per
root `CLAUDE.md` §1); `statusline/tests/` is the crate's own integration tier.
The lookup tests cover missing steps, type mismatches and malformed input; the
render tests pin the exact line for the real payload, and cover each degradation
above — no `rate_limits`, one window only, no `resets_at`, a reset in the past,
out-of-range percentages, and an empty payload. `src/status.rs` asserts the wire
format field by field and round-trips it; `src/nats.rs` covers `Settings` and the
two failure paths that must not hang — publishing disabled, and a server that is
not there; `src/link.rs` covers the backoff arithmetic and
every way the cache can be untrustworthy; `src/config.rs` covers a partial file,
each way a file can be broken, and checks the committed example against the
defaults. `src/activity.rs` covers the §9.10 document — each event's state, the
summary lifted from every shape of `tool_input`, the cut that must not split a
character, and the two directions of the dispatch (a hook payload is never
rendered, a status payload never publishes an activity document).

`tests/hook_dispatch.rs` runs the **actual executable** for every kind of
payload it can be handed and asserts what reaches stdout: a line for a status
payload or for garbage (§9.3), and nothing at all for any hook event. That last
one is why the test exists — on `Stop`, plain stdout is fed back to Claude as
context, so a status bar printed there would become something the model reads.
It writes a `publish: false` config next to the binary first, so it needs no
server and touches no subject.

`tests/nats_publish.rs` is the real round trip: publish through the same
blocking path `main` uses, receive it on a subscription, and read it back both
as the typed `Status` and as the untyped JSON a non-Rust subscriber sees. It
needs a live server, and mirrors pytest's `requires_nats` marker — Rust has no
"skip", so an unreachable server prints a note and returns green instead of
failing a suite on a machine where docker is down. `cargo test -- --nocapture`
shows whether it actually ran. It publishes to `status.test.statusline` and
`status.test.statusline.activity`, not `status` / `activity`, so it neither
disturbs a live subscriber nor can be fooled into passing by one.

### 9.7 The `status` subject — the same values, on the bus

The line answers the question for whoever is looking at that terminal. The same
numbers go onto NATS as JSON so something else can watch them: another machine,
a dashboard, a phone — the same reason the approval flow left the terminal in
the first place (root `CLAUDE.md`).

Every render publishes one message to **`status`** on the server from §3, the
one `lib/bus.py` talks to (`nats://127.0.0.1:4222`). Watch it with:

```
docker exec -it nats-box nats sub status
```

**The first real subscriber is `approver-web`**, which draws these numbers as a
plaque under the pending requests — see
[`../approver-web/CLAUDE.md`](../approver-web/CLAUDE.md), "The model and limits
plaque". It reads and never answers, so nothing here has to know about it; what it
does mean is that renaming a field below breaks a page as well as a test.

Core NATS, no stream: this is a *current value*, superseded a second later, and
persisting a history of it would be storage spent on nothing (§4). A subscriber
that was not listening missed it, by design.

```json
{
  "ts": 1786136782,
  "line": "Opus 5 (1M context) · high │ 5h █████░░░ 65% · 1h13m │ 7d ██░░░░░░ 27% · 4d7h │ ctx █░░░░░░░ 12%",
  "session_id": "7b463c0f-…",
  "cwd": "E:\\projects\\ai-remote",
  "model": {"id": "claude-opus-5[1m]", "display_name": "Opus 5 (1M context)"},
  "effort": {"level": "high"},
  "rate_limits": {
    "five_hour": {"used_percentage": 65.0, "resets_at": 1786141200,
                  "resets_in": 4418, "resets_in_text": "1h13m"},
    "seven_day": {"used_percentage": 27.0, "resets_at": 1786510800,
                  "resets_in": 374018, "resets_in_text": "4d7h"}
  },
  "context_window": {"used_percentage": 12.0}
}
```

- It is a **projection** of the payload, not a copy — the fields §9.1 reads,
  and nothing else that arrives on stdin.
- **Payload shape, not line shape.** `effort` is a sibling of `model` here because
  that is where it sits on stdin; pairing the two is a decision the *line* makes
  (§9.2), and a subscriber is free to make a different one.
- `line` travels with the numbers, colours stripped, so a subscriber can render
  nothing at all and still show something correct.
- `ts` is the clock the countdowns were resolved against, and `resets_in` /
  `resets_in_text` are that resolution — a subscriber in a container with a
  skewed clock gets the countdown without having to trust its own.
- Percentages are clamped 0–100 exactly like the bar, so the number on the bus
  and the number on screen can never disagree.
- **Absent is absent.** Missing sections are omitted, never `null` — a payload
  with no `rate_limits` (an API key rather than a subscription) publishes
  `{"ts":…,"line":"? │ limits n/a"}` and nothing more.

`src/status.rs` is that document as `serde` structs rather than a hand-built
`Value`: a rename in `render.rs` then breaks the build instead of quietly
breaking subscribers, and `Deserialize` lets the tests read the wire format back
without restating its shape.

**The line comes first, always.** This runs on every render, so a NATS server
that is down, slow, or simply not started must cost the user nothing:

- `main` prints the line and flushes stdout **before** it publishes.
- `nats::publish_blocking` bounds connect + publish + flush with **one** 500 ms
  timeout, builds a current-thread runtime, uses it, drops it.
- While the bus is known to be down it is not contacted at all — §9.8 is the
  part that actually makes an absent NATS free.
- Failures are a `Result` that `main` throws away. Set `"debug": true` in
  `statusline-config.json` (§9.9) to see them on stderr — off by default, because
  a server that is not running is the normal case here, not an incident.
- The flush is not optional: `nats pub` reporting success means "sent", not
  "delivered" (§4), and a one-shot process exits before an unflushed buffer
  drains — the same reason `registration_handler.py --once` flushes (§6).

All of it is configurable — see §9.9.

`src/nats.rs` is the access library, the Rust counterpart of `lib/bus.py`:
`Bus::connect` / `publish_json` / `flush` / `client()` for the async side, and
`publish_blocking(&Settings, &value)` for callers with no runtime. `Settings`
(url / subject / timeout / enabled) is the runtime shape; the file it comes from
is `config::Config`, which builds one with `Config::settings()` (§9.9).
Publishing only — nothing in the status line subscribes.

### 9.8 The dot, and why a dead NATS is free

Two problems with one answer — a cache file, `%TEMP%\ai-remote-statusline-link.json`.

**Drawing a dot for something not yet attempted.** The line is printed before
the publish (§9.7); reversing that would mean waiting on the network to see
your model name. So the dot shows what the *previous* render found. It lags by
one render — a fraction of a second on a bar that redraws every turn — and the
first run after the cache is cleared is always red. That is the trade, and the
alternative is a status line that blocks.

**Not paying for a server that is not there.** Refused on localhost is instant,
but a host that silently drops packets — a laptop off the VPN, a firewall —
costs the full timeout on *every* render. So a failure is remembered and not
retried for **30 s** (`link::DEFAULT_RETRY_AFTER`, overridable with
`retry_after_s` — §9.9). In between, publishing is skipped before any socket is
opened and the render costs one small file read. Measured against an unroutable
address:

| Render | Cost |
|--------|------|
| first, cache cold | ~750 ms — the connect timeout, paid once |
| every render for the next 30 s | ~23 ms — process startup, no network at all |

While *connected* there is no backoff: every render publishes, which is the
whole point, and a successful localhost round trip is single-digit milliseconds.

The cache holds `{connected, checked_at, url}`. Missing, unreadable, corrupt, or
recorded against a **different** `url` than the config now names all read as "unknown" —
red dot, retry now — because none of those is worth a message in a status bar,
and another project's verdict about another server is not evidence about this
one. A `checked_at` in the future (the clock moved, the file was copied between
machines) is treated as stale rather than trusted, so it cannot suppress
retries indefinitely. It lives in the temp directory, not the repo: it is a
cache, rewritten several times a minute, and losing it costs one red dot.

### 9.9 `statusline-config.json`

Everything configurable, in one file **next to the executable** — so with the
default wiring, `statusline/target/release/statusline-config.json`. Read fresh
on every render, so an edit takes effect on the next turn with no restart.

Not the environment, which is where these settings started: `settings.json`
hands a status line command no arguments and no variables of its own, so tuning
it meant editing a shell profile and restarting the terminal to change a subject
name. Not the repository either — the binary is what gets copied to another
machine, and its configuration should travel with it.

`statusline-config.example.json` in the **repository root** is the committed
copy of the format, and it spells out the defaults:

```json
{
  "v": 1,
  "publish": true,
  "url": "nats://127.0.0.1:4222",
  "subject": "status",
  "activity": true,
  "activity_subject": "activity",
  "timeout_ms": 500,
  "retry_after_s": 30,
  "debug": false
}
```

| Field | Default | Meaning |
|-------|---------|---------|
| `v` | `1` | schema version, as in `lib/config.py` |
| `publish` | `true` | publish at all. `false` also removes the dot (§9.8) — nothing is published, so there is no link to report |
| `url` | `nats://127.0.0.1:4222` | the server (§3) |
| `subject` | `status` | the subject (§9.7) |
| `activity` | `true` | publish the activity documents (§9.10). A switch of its own because that is the half putting command text and file paths on the bus; `publish: false` still switches off both |
| `activity_subject` | `activity` | where those go (§9.10) |
| `timeout_ms` | `500` | budget for the whole publish — connect, write, flush |
| `retry_after_s` | `30` | how long a failed connection is believed before a retry (§9.8) |
| `debug` | `false` | print publish failures to stderr |

**The file is optional and so is every field in it.** A fresh
`cargo build --release` has no config beside it and publishes to `status` on
localhost regardless; a file may set only what it changes.

**Nothing in it can break the line.** Missing, unreadable, malformed, holding a
misspelled key, or stamped with a `v` this build does not know — every one of
those falls back to the defaults in full. `lib/config.py` refuses to load in
that situation and is right to: a half-understood approval config is a security
question. A half-understood status line config is a cosmetic one, and the line
has to print either way. The cost of that leniency is that a typo is silent, so
`debug: true` is the way to find out whether the file is being read at all.

A test asserts the committed example parses and equals the defaults, so the
documentation above cannot drift away from the code without failing the suite.

### 9.10 The `activity` subject — what Claude is *doing*

§9.7 puts the numbers on the bus: what this session is **spending**. This puts
the other half there — what it is **doing** — and it arrives on the events the
numbers cannot see. A status line is re-rendered on a timer and a turn boundary;
a tool call is neither.

Three events, published from the same binary wired as three hooks (§9.5):

| Claude Code event | `event` | `state` | Means |
|---|---|---|---|
| `PreToolUse` | `pre_tool` | `running` | a tool is about to run, and the document says which |
| `PostToolUse` | `post_tool` | `thinking` | that tool is done; the turn is not |
| `Stop` | `stop` | `idle` | the turn ended — the session is waiting for a human |

`post_tool` is deliberately **not** `idle`: between two tool calls Claude is
working, and the only event that means "your turn" is `Stop`. `state` is
derivable from `event` and sent anyway, because it is what a readout actually
draws and a subscriber should not have to hard-code the mapping to colour a dot.

One message per event, on **`activity`** (`activity_subject`, §9.9). Core NATS,
no stream, same as §9.7: a current value, superseded by the next one, and a
subscriber that was not listening missed it by design. Watch it with:

```
docker exec -it nats-box nats sub activity
```

```json
{
  "v": 1,
  "ts": 1786136782,
  "event": "pre_tool",
  "state": "running",
  "session_id": "7b463c0f-…",
  "cwd": "E:\\projects\\ai-remote",
  "tool_name": "Bash",
  "summary": "py -m pytest -q",
  "tool_use_id": "toolu_01ABC123",
  "agent_type": "Explore"
}
```

- **`v` is required, and §9.7's document has no such field.** Not an
  inconsistency: `status` always carries `ts` **and** `line`, which is enough for
  a subscriber on an open subject to recognise it. Here everything but `ts`,
  `event` and `state` may be absent, so there is no such pair — `v` is both the
  version and the "this is ours" marker, and a `v: 2` is refused rather than
  half-understood.
- **`ts` is the only clock.** A hook fires when it fires; there is nothing to
  count down and no `resets_in` to resolve.
- **Absent is absent**, exactly as in §9.7 — omitted, never `null`. A `stop`
  document is four fields: `{"v":1,"ts":…,"event":"stop","state":"idle"}` plus
  the session it came from.
- `tool_use_id` is Claude Code's own id for the call, so a `post_tool` is matched
  to the `pre_tool` it closes instead of guessed at by tool name.
- `agent_type` appears only inside a subagent, so a readout can say *whose* work
  this is (`Explore › Grep · TODO`) rather than showing the main loop doing
  everything.

**A summary, not the arguments.** A tool's input is command text, file paths,
sometimes a secret; a row on a phone or a 2.16" panel wants one short line. So
exactly one value is lifted out of `tool_input`, whitespace — newlines included,
since a `Bash` heredoc is one value with many lines — collapsed to single spaces,
and the result cut to **80 characters** on a character boundary, with `…` where
it was cut. The key is chosen by a preference order over names rather than a
table of tools:

```
command · file_path · notebook_path · pattern · path · url · query · skill · description · prompt
```

Names, not tools, for two reasons: a per-tool table would have to grow with every
tool Claude Code adds, and it would say nothing at all about an MCP tool — while
an unknown tool that happens to take a `command` or a `path` is summarised
correctly by nobody's effort. `pattern` sits ahead of `path` because for `Grep`
and `Glob` the pattern is the question and the path is only where it was asked.
Nothing matched (`TodoWrite`) means no `summary` at all, not an empty one.

**What never travels.** `last_assistant_message` — the model's own prose, which
`Stop` carries in full — is not published. Neither is `tool_result`, nor
`transcript_path`, nor the rest of `tool_input` beyond that one line. This is a
projection of the payload in the same sense §9.7 is: the fields a readout needs,
and nothing else.

**One binary, two jobs, and the dispatch is the risky part.** Claude Code hands
the same executable a status payload or a hook payload, and every hook payload
carries `hook_event_name` while the status one does not. So `main` reads stdin,
asks `activity::is_hook_payload`, and only then decides whether anything may be
printed. Two rules fall out of that, and `tests/hook_dispatch.rs` pins both:

- **An event we do not publish produces nothing at all** — not a status line.
  `SessionStart`, `SubagentStop`, `PermissionRequest` (that one is
  `approver/hook.py`'s job, §7) all leave the process having done nothing, which
  is exactly what a hook with no opinion should do. Printing a line there would be
  worse than noise: on `Stop`, plain stdout is handed back to Claude as context,
  so the status bar would become something the model reads.
- **A status payload is never mistaken for a hook.** If some later Claude Code
  starts stamping the status payload with a name of its own, `Status` and
  `StatusLine` are excluded by name, so the line keeps printing instead of being
  silently replaced by a publish.

**What it costs.** A hook runs on every tool call — twice — so this is only
affordable because it reuses §9.8: while the server is known to be down, no
socket is opened at all. Measured here against a live localhost server, release
build:

| Run | Cost |
|-----|------|
| first (cold file cache) | ~240 ms |
| every later hook | ~31–38 ms — process start, connect, publish, flush |
| any hook while the bus is known down | the §9.8 file read, no network |

The §9.8 cache is shared with the line, which is a feature in both directions:
hooks keep the connection verdict fresh between renders, and a hook that fails to
connect turns the next render's dot red — it is the same server.

**It is command text on a LAN bus, and that is a decision.** Everything above
travels unencrypted to whoever is subscribed, exactly like a §7 request does
(root `CLAUDE.md` §7: the bus must not be exposed). Two switches, in
`statusline-config.json` (§9.9): `activity: false` keeps the line publishing and
stops this, and `publish: false` stops both.

**The first subscriber is `approver-web`**, which draws it as one line under the
limits plaque — see [`../approver-web/CLAUDE.md`](../approver-web/CLAUDE.md),
"The activity row". Read-only, never answered, and nothing in the approval flow
depends on it.
