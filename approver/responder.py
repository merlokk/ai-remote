"""responder.py — human approval responder (CLAUDE.md §6/§7).

Two commands:

  register <token>   Generate a fresh Ed25519 key pair, register its public half
                     with the registration handler over ``registrations`` using a
                     one-time token, and store the pair in responder-config.json.
                     The key is persisted only if the handler acks ``ok:true``, so
                     a rejected registration never clobbers a working config.

  serve              Subscribe to ``approvals.*``, present each request to the
                     operator, sign the decision (Ed25519) and reply. A queue group
                     keeps a single responder answering when several are running.

Run with the `py` launcher (CLAUDE.md §5):  py approver/responder.py serve
"""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

# Non-package project: make repo-root imports (`lib`, `approver`) work when this
# file is run directly as a script (script dir alone is not enough).
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from approver import protocol  # noqa: E402
from lib import bus  # noqa: E402
from lib import config as configlib  # noqa: E402
from lib import crypto  # noqa: E402

DEFAULT_CONFIG = Path(__file__).resolve().parent / "responder-config.json"
DEFAULT_SUBJECT = "approvals.*"
DEFAULT_QUEUE = "approvers"
_BEHAVIORS = ("allow", "deny")


# --- pure helpers --------------------------------------------------------------
def parse_key_id(token: str) -> str:
    """Extract ``key_id`` from a ``<key_id>.<secret>`` token (§6). First dot splits."""
    if "." not in token:
        raise ValueError("token must be '<key_id>.<secret>'")
    key_id = token.split(".", 1)[0]
    if not key_id:
        raise ValueError("token has an empty key_id")
    return key_id


def build_registration_request(token: str, pubkey_b64: str, ts: int) -> dict:
    """Assemble the ``registrations`` request; ``key_id`` comes from the token (§6)."""
    return {
        "v": protocol.PROTOCOL_VERSION,
        "token": token,
        "key_id": parse_key_id(token),
        "pubkey": pubkey_b64,
        "ts": ts,
    }


def build_reply(
    request: dict,
    *,
    behavior: str,
    key_id: str,
    private_b64: str,
    reason: str = "",
    updated_input: dict | None = None,
) -> dict:
    """Build a signed reply for ``request`` (§7).

    Echoes ``v/session_id/tool_name/input_sha256/nonce/ts`` from the request; the
    responder contributes ``behavior/reason/updated_input``. ``updated_input`` is
    honored only on ``allow``. The signature covers the recomputed
    ``updated_input_sha256`` — the hash itself is not sent on the wire.
    """
    if behavior not in _BEHAVIORS:
        raise ValueError(f"behavior must be one of {_BEHAVIORS}, got {behavior!r}")

    apply_update = behavior == "allow" and updated_input is not None
    updated_input_sha256 = protocol.canonical_sha256(updated_input) if apply_update else ""

    sb = protocol.signing_bytes(
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
    sig = crypto.sign(private_b64, sb)

    reply = {
        "v": request["v"],
        "behavior": behavior,
        "reason": reason,
        "session_id": request["session_id"],
        "tool_name": request["tool_name"],
        "input_sha256": request["input_sha256"],
        "nonce": request["nonce"],
        "ts": request["ts"],
        "key_id": key_id,
        "sig": sig,
    }
    if apply_update:
        reply["updated_input"] = updated_input
    return reply


# --- commands ------------------------------------------------------------------
async def register(
    token: str,
    *,
    config_path: Path | str = DEFAULT_CONFIG,
    servers: str = bus.DEFAULT_SERVERS,
    timeout: float = 10.0,
) -> dict:
    """Register/rotate this responder's key. Persists only on ``ok:true``.

    Returns the handler's reply dict. Raises ``ValueError`` on a malformed token
    and ``bus.BusError`` on transport failure (no handler / timeout).
    """
    key_id = parse_key_id(token)
    keypair = crypto.generate_keypair()
    req = build_registration_request(token, keypair.public_b64(), int(time.time()))

    async with bus.connect(servers) as b:
        reply = await b.request("registrations", req, timeout=timeout)

    if reply.get("ok"):
        cfg = configlib.Config(
            config_path,
            {
                "v": protocol.PROTOCOL_VERSION,
                "key_id": key_id,
                "private_key": keypair.private_b64(),
                "public_key": keypair.public_b64(),
            },
        )
        cfg.save()
    return reply


def prompt_operator(request: dict):
    """Blocking console prompt. Returns ``(behavior, reason, updated_input)`` or None to skip."""
    print("\n=== permission request ===================================", file=sys.stderr)
    print(f"  session : {request.get('session_id')}", file=sys.stderr)
    print(f"  tool    : {request.get('tool_name')}", file=sys.stderr)
    print(f"  cwd     : {request.get('cwd')}", file=sys.stderr)
    print(f"  mode    : {request.get('permission_mode')}", file=sys.stderr)
    print(
        "  input   : "
        + json.dumps(request.get("tool_input"), ensure_ascii=False, indent=2),
        file=sys.stderr,
    )
    print("===========================================================", file=sys.stderr)
    answer = input("allow / deny / skip? [a/d/s]: ").strip().lower()
    if answer in ("a", "allow"):
        return ("allow", input("reason (optional): ").strip(), None)
    if answer in ("d", "deny"):
        return ("deny", input("reason (optional): ").strip(), None)
    print("skipped (no reply — hook falls back to the interactive prompt)", file=sys.stderr)
    return None


async def serve(
    *,
    config_path: Path | str = DEFAULT_CONFIG,
    servers: str = bus.DEFAULT_SERVERS,
    subject: str = DEFAULT_SUBJECT,
    queue: str = DEFAULT_QUEUE,
    prompt=prompt_operator,
) -> None:
    """Answer approval requests until interrupted."""
    cfg = configlib.Config.load(config_path)
    if "key_id" not in cfg or "private_key" not in cfg:
        raise configlib.ConfigError(
            f"{config_path} has no key — run: responder.py register <token>"
        )
    key_id = cfg["key_id"]
    private_b64 = cfg["private_key"]

    async def handler(request: dict):
        # Run the blocking prompt off the event loop so NATS keeps its heartbeats.
        decision = await asyncio.to_thread(prompt, request)
        if not decision:
            return None
        behavior, reason, updated_input = decision
        return build_reply(
            request,
            behavior=behavior,
            key_id=key_id,
            private_b64=private_b64,
            reason=reason,
            updated_input=updated_input,
        )

    async with bus.connect(servers) as b:
        await b.reply(subject, handler, queue=queue)
        print(
            f"responder key_id={key_id!r} serving {subject!r} "
            f"(queue={queue!r}) — Ctrl+C to stop",
            file=sys.stderr,
        )
        await asyncio.Event().wait()  # run until cancelled / interrupted


# --- CLI -----------------------------------------------------------------------
def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="responder.py", description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_reg = sub.add_parser("register", help="register/rotate key via a one-time token")
    p_reg.add_argument("token", help="one-time token '<key_id>.<secret>' from the handler")
    p_reg.add_argument("--config", default=str(DEFAULT_CONFIG))
    p_reg.add_argument("--servers", default=bus.DEFAULT_SERVERS)
    p_reg.add_argument("--timeout", type=float, default=10.0)

    p_srv = sub.add_parser("serve", help="answer approval requests")
    p_srv.add_argument("--config", default=str(DEFAULT_CONFIG))
    p_srv.add_argument("--servers", default=bus.DEFAULT_SERVERS)
    p_srv.add_argument("--subject", default=DEFAULT_SUBJECT)
    p_srv.add_argument("--queue", default=DEFAULT_QUEUE)
    return parser


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)

    if args.cmd == "register":
        try:
            reply = asyncio.run(
                register(
                    args.token,
                    config_path=Path(args.config),
                    servers=args.servers,
                    timeout=args.timeout,
                )
            )
        except (ValueError, bus.BusError) as e:
            print(f"registration failed: {e}", file=sys.stderr)
            return 1
        if reply.get("ok"):
            print(f"registered key_id={reply.get('key_id')} (config: {args.config})")
            return 0
        print(f"registration rejected: {reply.get('error')}", file=sys.stderr)
        return 1

    if args.cmd == "serve":
        try:
            asyncio.run(
                serve(
                    config_path=Path(args.config),
                    servers=args.servers,
                    subject=args.subject,
                    queue=args.queue,
                )
            )
        except KeyboardInterrupt:
            return 0
        except (configlib.ConfigError, bus.BusError) as e:
            print(f"serve failed: {e}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
