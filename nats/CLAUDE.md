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
| `status` | publish only, no stream | [`statusline/`](../statusline/CLAUDE.md) §9.7 |
| `status.test.statusline` | the Rust integration test, so it cannot disturb a live subscriber | [`statusline/`](../statusline/CLAUDE.md) §9.6 |

**The bus is unauthenticated and every subject on it is open.** That is
acceptable only because it is bound to localhost in this sandbox: `approvals.*`
carries the full `tool_input` (for Bash, the whole command; for Write, the file
contents), and anyone who can publish can answer a registration request — which
is why the §6 replies are signed and the §7 replies are verified rather than
trusted. Do not expose these ports.
