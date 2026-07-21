"""hook.py — Claude Code PermissionRequest hook → signed NATS approval (§7).

Reads the hook payload on stdin, asks a human responder over NATS for a signed
allow/deny, verifies the signature against the trusted-key allowlist
(`handler-config.json` → ``clients``), and prints the decision on stdout.

Fail-safe is the whole point: NATS down, a timeout, a bad/absent signature, a
mismatched echo field, or an untrusted ``key_id`` all lead to a non-blocking
exit (≠0 and ≠2) so Claude Code falls back to the interactive prompt. The
allow/deny decision is only ever delivered via exit-0 JSON — never exit 2, and
never a silent allow.

Wire it up (settings.json) as a PermissionRequest hook, matcher ``*``:
  py E:\\projects\\ai-remote\\approver\\hook.py
The config file location comes from AI_REMOTE_HANDLER_CONFIG (or ``--config``); the
NATS server(s) and approval timeout are read from that config's ``servers`` /
``timeout`` keys (falling back to the defaults below when absent).
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
import secrets
import sys
import time
from pathlib import Path

# Non-package project: make repo-root imports work when run directly as a script.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from approver import protocol  # noqa: E402
from lib import bus  # noqa: E402
from lib import config as configlib  # noqa: E402
from lib import crypto  # noqa: E402

HOOK_EVENT = "PermissionRequest"
DEFAULT_CONFIG = Path(__file__).resolve().parent / "handler-config.json"
DEFAULT_TIMEOUT = 60.0  # generous: a human has to read and decide
_NONCE_BYTES = 32
_BEHAVIORS = ("allow", "deny")
# Echoed fields whose values must come back unchanged (anti-replay + command binding).
_ECHO_FIELDS = ("v", "session_id", "tool_name", "input_sha256", "nonce", "ts")


class HookError(Exception):
    """A reply could not be trusted; the caller must fall back to the prompt."""


def _new_nonce() -> str:
    return base64.b64encode(secrets.token_bytes(_NONCE_BYTES)).decode("ascii")


def build_request(payload: dict, *, nonce: str, ts: int) -> dict:
    """Assemble the ``approvals.<session_id>`` request from the hook payload (§7)."""
    tool_input = payload.get("tool_input", {})
    return {
        "v": protocol.PROTOCOL_VERSION,
        "session_id": payload["session_id"],
        "tool_name": payload["tool_name"],
        "tool_input": tool_input,
        "input_sha256": protocol.canonical_sha256(tool_input),
        "permission_mode": payload.get("permission_mode"),
        "cwd": payload.get("cwd"),
        "nonce": nonce,
        "ts": ts,
    }


def verify_reply(request: dict, reply, allowlist: dict) -> tuple[bool, str]:
    """Return ``(trusted, reason)`` for ``reply`` against the sent ``request``.

    Order matters: cheap echo/allowlist checks precede the signature check. Only
    after ``sig`` verifies may ``behavior``/``reason``/``updated_input`` be trusted.
    """
    if not isinstance(reply, dict):
        return False, "reply is not an object"

    for field in _ECHO_FIELDS:
        if reply.get(field) != request.get(field):
            return False, f"{field} does not match the request"
    if reply.get("v") != protocol.PROTOCOL_VERSION:
        return False, "unexpected protocol version"

    behavior = reply.get("behavior")
    if behavior not in _BEHAVIORS:
        return False, f"invalid behavior {behavior!r}"

    key_id = reply.get("key_id")
    client = allowlist.get(key_id) if isinstance(key_id, str) else None
    if not isinstance(client, dict) or not client.get("pubkey"):
        return False, f"key_id {key_id!r} not in allowlist"

    reason = reply.get("reason", "")
    if not isinstance(reason, str):
        return False, "reason is not a string"

    updated_input = reply.get("updated_input")
    if behavior == "allow" and updated_input is not None:
        updated_input_sha256 = protocol.canonical_sha256(updated_input)
    else:
        updated_input_sha256 = ""

    signing_bytes = protocol.signing_bytes(
        v=request["v"],
        session_id=request["session_id"],
        nonce=request["nonce"],
        tool_name=request["tool_name"],
        input_sha256=request["input_sha256"],
        behavior=behavior,
        updated_input_sha256=updated_input_sha256,
        ts=request["ts"],
        reason=reason,
    )
    sig = reply.get("sig")
    if not isinstance(sig, str) or not crypto.verify(client["pubkey"], signing_bytes, sig):
        return False, "signature verification failed"

    return True, "ok"


def decision_output(reply: dict) -> dict:
    """Build the exit-0 hook JSON. ``updatedInput`` is emitted only on allow."""
    decision: dict = {"behavior": reply["behavior"]}
    if reply["behavior"] == "allow" and isinstance(reply.get("updated_input"), dict):
        decision["updatedInput"] = reply["updated_input"]
    return {"hookSpecificOutput": {"hookEventName": HOOK_EVENT, "decision": decision}}


def _load_config_data(config_path: Path | str) -> dict:
    """Load the handler config (allowlist + optional hook settings), or defaults."""
    return configlib.Config.load(
        config_path, default={"v": protocol.PROTOCOL_VERSION, "clients": {}}
    ).data


def allowlist_from_config(data: dict) -> dict:
    """The trusted-key allowlist (``clients``) from loaded config data."""
    clients = data.get("clients", {})
    return clients if isinstance(clients, dict) else {}


def servers_from_config(data: dict) -> str:
    """NATS server(s) for the hook — config ``servers`` key, else the default."""
    servers = data.get("servers")
    return servers if isinstance(servers, str) and servers else bus.DEFAULT_SERVERS


def timeout_from_config(data: dict) -> float:
    """Approval timeout in seconds — config ``timeout`` key, else the default."""
    timeout = data.get("timeout")
    # bool is an int subclass; a stray true/false is not a valid timeout.
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)):
        return DEFAULT_TIMEOUT
    return float(timeout) if timeout > 0 else DEFAULT_TIMEOUT


async def request_decision(
    payload: dict,
    *,
    config_path: Path | str = DEFAULT_CONFIG,
    servers: str | None = None,
    timeout: float | None = None,
) -> dict:
    """Round-trip one approval and return the hook JSON, or raise on any failure.

    ``servers`` / ``timeout`` default to the values in the handler config (the
    ``servers`` / ``timeout`` keys); pass them explicitly only to override.
    """
    data = _load_config_data(config_path)
    allowlist = allowlist_from_config(data)
    if servers is None:
        servers = servers_from_config(data)
    if timeout is None:
        timeout = timeout_from_config(data)

    request = build_request(payload, nonce=_new_nonce(), ts=int(time.time()))

    async with bus.connect(servers) as b:
        reply = await b.request(f"approvals.{payload['session_id']}", request, timeout=timeout)

    trusted, reason = verify_reply(request, reply, allowlist)
    if not trusted:
        raise HookError(reason)
    return decision_output(reply)


def _parse_args(argv):
    parser = argparse.ArgumentParser(prog="hook.py", description="PermissionRequest → NATS approval")
    # servers/timeout now live in the handler config (its `servers`/`timeout`
    # keys); only the config location itself is an argument / env var.
    parser.add_argument(
        "--config", default=os.environ.get("AI_REMOTE_HANDLER_CONFIG", str(DEFAULT_CONFIG))
    )
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = _parse_args(argv)

    try:
        payload = json.loads(sys.stdin.read())
    except (json.JSONDecodeError, ValueError) as e:
        print(f"hook: unreadable stdin, falling back to prompt: {e}", file=sys.stderr)
        return 1

    if not isinstance(payload, dict) or payload.get("hook_event_name") != HOOK_EVENT:
        # Not our event — let Claude Code handle it normally.
        print("hook: not a PermissionRequest payload, falling back to prompt", file=sys.stderr)
        return 1

    try:
        output = asyncio.run(
            request_decision(payload, config_path=Path(args.config))
        )
    except Exception as e:  # noqa: BLE001 — fail-safe: ANY error → interactive prompt
        print(f"hook: falling back to prompt: {e}", file=sys.stderr)
        return 1

    json.dump(output, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
