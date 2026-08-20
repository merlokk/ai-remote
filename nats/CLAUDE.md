# nats/ — the local NATS sandbox

The bus everything else in this repository talks to: one `docker compose` file
bringing up the NATS server, a web dashboard to watch it, and a `nats-box`
container with the `nats` CLI for manual checks.

Nothing here is application code — the Python side reaches it through
[`lib/bus.py`](../lib/CLAUDE.md), the Rust status line through
[`statusline/src/nats.rs`](../statusline/CLAUDE.md) §9.7, and the approval
protocol that runs over it is §6–§7 in
[`approver/CLAUDE.md`](../approver/CLAUDE.md).

This file owns sections **3** and **4**. The numbering is global — see
[`../CLAUDE.md`](../CLAUDE.md) §2 for the map — and both numbers are cited from
code (`tests/conftest.py`, `statusline/src/nats.rs`) and from other folder docs,
so they keep their numbers wherever the text lives.

## 3. Infrastructure (`nats/docker-compose.yml`)

Bring up: `cd nats && docker compose up -d`

| Service          | Container        | Ports (host→container)          | Purpose                                                                   |
|------------------|------------------|---------------------------------|---------------------------------------------------------------------------|
| `nats`           | `nats-server`    | 4222→4222, 8222→8222, 6222→6222 | client; HTTP monitoring (8222 — `/varz`, `/jsz`, `/connz`); clustering    |
| `nats-dashboard` | `nats-dashboard` | 8080→**80**                     | Web UI (http://localhost:8080/)                                           |
| `nats-box`       | `nats-box`       | —                               | `nats` CLI (`docker exec -it nats-box sh`)                                |

JetStream data lives on the named volume `nats_data` (mounted at `/data`, server started with `--store_dir=/data`), so streams survive `docker compose down`; `docker compose down -v` wipes them.

**The server runs with `--max_payload=65536`, and that is a fail-safe for a
device rather than a preference.** The default is 1 MB; the ESP32 responder
([`approver-esp32/protocol.md`](../approver-esp32/protocol.md) §10.5) drops its socket on any
message whose receive buffer its client library cannot allocate, which starts at
128 KB — so on the default, anyone on this LAN could publish 1 MB to `approvals.*`
in a loop and keep the responder reconnecting forever, with a real permission
request never delivered. Refusing the publisher is the right end to fail at.

The cost is on the other side, and is worth knowing before it surprises somebody:
`hook.py` puts the whole `tool_input` on the wire, so **a `Write` of more than
64 KB cannot be approved through this bus.** The publish is refused, the hook
times out, and Claude Code falls back to asking in its own terminal (§7) — the
fail-safe working rather than a bug, but from the outside it looks like the
responder being down. `curl -s localhost:8222/varz` reports what the server is
actually running with, which is the check worth doing before believing this
paragraph.

## 4. NATS: key concepts

- `-js` only **enables** JetStream, it does not turn on persistence globally.
- Persistence is targeted: via a **stream** that captures the given subjects (`nats stream add ORDERS --subjects "orders.*"`). Subjects without a stream behave like Core NATS (fire-and-forget).
- `nats pub` prints "Published" = confirmation of sending, NOT of delivery/storage.

Both matter downstream, and both are cited rather than restated: the approval
flow is Core request-reply on purpose (§7), `registration_handler.py --once`
flushes before draining because "sent" is not "delivered" (§6), and the status
line publishes a current value with no stream behind it (§9.7).

## What runs on this bus

| Subject | Direction | Owner |
|---------|-----------|-------|
| `approvals.<session_id>` | request-reply, queue group `approvers` | [`approver/`](../approver/CLAUDE.md) §7 |
| `registrations` | request-reply | [`approver/`](../approver/CLAUDE.md) §6 |
| `status` | publish only, no stream — what the session is **spending** | [`statusline/`](../statusline/CLAUDE.md) §9.7 |
| `activity` | publish only, no stream — what it is **doing**: one message per `PreToolUse` / `PostToolUse` / `Stop` | [`statusline/`](../statusline/CLAUDE.md) §9.10 |
| `status.test.statusline` | the Rust integration test, so it cannot disturb a live subscriber | [`statusline/`](../statusline/CLAUDE.md) §9.6 |
| `status.test.statusline.activity` | the same, for the other document | [`statusline/`](../statusline/CLAUDE.md) §9.6 |

The two read-only subjects are published by the same binary and are **not**
request-reply: a current value with nothing behind it, so a subscriber that was
not listening missed it by design (§4). Both are read by
[`approver-web/`](../approver-web/CLAUDE.md) and by the ESP32's limits screen
([`approver-esp32/screens.md`](../approver-esp32/screens.md) §10.8.3), neither of
which ever answers — and both are subscribed **without a queue group**, unlike
`approvals.*`, because a broadcast is meant to reach every watcher rather than
exactly one.

**The bus is unauthenticated and every subject on it is open.** `approvals.*`
carries the full `tool_input` (for Bash, the whole command; for Write, the file
contents), and anyone who can publish can answer a registration request — which
is why the §6 replies are signed and the §7 replies are verified rather than
trusted.

**The client port is deliberately reachable from the home LAN** — decided so the
ESP32 responder can reach it over Wi-Fi ([`approver-esp32/`](../approver-esp32/CLAUDE.md)
§10.3, in that folder's own `CLAUDE.md`), which is where the trade-off is argued. Read it as one sentence: *every
device on that Wi-Fi can read every permission request in cleartext, and the
protocol survives that because a decision still cannot be forged.* The boundary
is now the router, not loopback, so three things hold it up:

- **Nothing forwards a port to this host.** No port-forward, no UPnP hole, no
  DMZ — that is the whole thing standing between `approvals.*` and the internet
  now. Worth verifying once, deliberately, rather than assuming.
- **`4222` is the only port that needs to leave the machine.** The monitoring
  port `8222` (`/varz`, `/connz`, `/jsz`) and the dashboard on `8080` have no
  device on the other end; bind them to `127.0.0.1` in `docker-compose.yml`
  rather than leaving them on every interface.
- **`4222:4222` means every interface, including ones joined later.** A VPN or
  an overlay network (Tailscale, ZeroTier, a corporate tunnel) the host joins
  next month carries this bus onto it silently. Publishing on the LAN address
  explicitly (`192.168.x.y:4222:4222`) is the cheap version of not finding that
  out later.

TLS + credentials remain the real fix and are not done. The moment this bus
crosses a network that is not "the flat inside a router" — a shared office, a
guest VLAN with other people's devices, anything routed — it needs them first.
