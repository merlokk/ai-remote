"""responder.py — human approval responder (CLAUDE.md §6/§7).

Two commands:

  register <token>   Generate a fresh key pair — Ed25519 by default, ECDSA P-256
                     with ``--key-type p256`` — register its public half with the
                     registration handler over ``registrations`` using a one-time
                     token, and store the pair in responder-config.json. The key is
                     persisted only if the handler acks ``ok:true``, so a rejected
                     registration never clobbers a working config. The handler
                     signs its reply with the server key (§6); that signature is
                     verified first and the key is pinned in the config, so no one
                     else on the open ``registrations`` subject can answer for it.

  serve              Subscribe to ``approvals.*``, present each request to the
                     operator, sign the decision with the config's ``key_type``
                     (ed25519/p256) and reply. A queue group keeps a single
                     responder answering when several are running.

Run with the `py` launcher (CLAUDE.md §5):  py approver/responder.py serve
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import json
import secrets
import sys
import time
from collections.abc import Callable
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
_NONCE_BYTES = 32


class ServerReplyError(Exception):
    """A reply from the registration handler could not be trusted (§6).

    Raised instead of returning it: an unsigned, mis-signed or replayed answer is
    not a rejection to report, it is an answer from someone who is not the
    allowlist owner — and acting on it (in either direction) is the mistake.
    """


# --- pure helpers --------------------------------------------------------------
def new_nonce() -> str:
    """A fresh registration nonce (base64, 32 random bytes).

    Echoed by the handler inside its signature, so an old reply cannot be replayed
    at a later registration.
    """
    return base64.b64encode(secrets.token_bytes(_NONCE_BYTES)).decode("ascii")


def parse_key_id(token: str) -> str:
    """Extract ``key_id`` from a ``<key_id>.<secret>`` token (§6). First dot splits."""
    if "." not in token:
        raise ValueError("token must be '<key_id>.<secret>'")
    key_id = token.split(".", 1)[0]
    if not key_id:
        raise ValueError("token has an empty key_id")
    return key_id


def build_registration_request(
    token: str,
    pubkey_b64: str,
    ts: int,
    key_type: str = crypto.DEFAULT_KEY_TYPE,
    nonce: str = "",
) -> dict:
    """Assemble the ``registrations`` request; ``key_id`` comes from the token (§6).

    ``key_type`` (``ed25519``/``p256``) tells the handler which algorithm to pin
    for this key in the allowlist; the hook then verifies with that same scheme.

    ``nonce`` comes back inside the handler's signature over its reply, which is
    what makes that reply usable exactly once, for this exchange.
    """
    return {
        "v": protocol.PROTOCOL_VERSION,
        "token": token,
        "key_id": parse_key_id(token),
        "pubkey": pubkey_b64,
        "key_type": key_type,
        "nonce": nonce,
        "ts": ts,
    }


def verify_server_reply(reply, *, nonce: str, pinned_pubkey: str | None = None) -> str:
    """Check the registration handler's signature over ``reply``; return its key.

    ``registrations`` is an open subject, so a reply is only worth reading once it
    is known to come from the allowlist owner. Checked here: the protocol version,
    the ``nonce`` echo (this exchange, not an old one), and an Ed25519 signature
    over :func:`approver.protocol.registration_reply_signing_bytes` — which covers
    ``ok``, ``key_id`` and ``error``, so a rejection cannot be turned into an
    acceptance or the other way round.

    ``pinned_pubkey`` is the key this approver already trusts (from its own
    config). When set, the reply must be signed by exactly that key: a valid
    signature by a *different* key is the takeover this pinning exists to stop.
    Without it — the first registration — the key is taken on trust and pinned for
    every registration after; compare it once with what the handler printed.

    Raises :class:`ServerReplyError` on anything it cannot vouch for.
    """
    if not isinstance(reply, dict):
        raise ServerReplyError("reply is not an object")
    if reply.get("v") != protocol.PROTOCOL_VERSION:
        raise ServerReplyError("unexpected protocol version")

    ok = reply.get("ok")
    if not isinstance(ok, bool):
        raise ServerReplyError("reply has no 'ok' flag")

    server_key = reply.get("server_key")
    if not isinstance(server_key, str) or not server_key:
        raise ServerReplyError(
            "reply carries no server key - is the registration handler up to date?"
        )
    if pinned_pubkey and server_key != pinned_pubkey:
        raise ServerReplyError(
            "the reply is signed by a different key than the registration handler this "
            "approver already trusts - refusing to re-pin it"
        )

    if reply.get("nonce") != nonce:
        raise ServerReplyError("nonce does not match the request (a replayed reply?)")

    ts = reply.get("ts")
    if isinstance(ts, bool) or not isinstance(ts, int):
        raise ServerReplyError("reply has no timestamp")

    key_id, error = reply.get("key_id", ""), reply.get("error", "")
    if not isinstance(key_id, str) or not isinstance(error, str):
        raise ServerReplyError("reply has a non-string 'key_id'/'error'")

    sig = reply.get("sig")
    signing_bytes = protocol.registration_reply_signing_bytes(
        v=protocol.PROTOCOL_VERSION, ok=ok, key_id=key_id, nonce=nonce, ts=ts, error=error
    )
    if not isinstance(sig, str) or not crypto.verify(
        server_key, signing_bytes, sig, protocol.SERVER_KEY_TYPE
    ):
        raise ServerReplyError("the registration handler's signature does not verify")

    return server_key


def pinned_server_key(config_path: Path | str) -> str | None:
    """The handler public key this approver already trusts, or None if it has none.

    Reads any approver config (they all store it under the same key), and treats an
    unreadable one as "nothing pinned" — registration is about to overwrite it.
    """
    try:
        cfg = configlib.Config.load(config_path)
    except configlib.ConfigError:
        return None
    server_key = cfg.get("server_key")
    return server_key if isinstance(server_key, str) and server_key else None


def build_signed_reply(
    request: dict,
    *,
    behavior: str,
    key_id: str,
    sign: Callable[[bytes], str],
    reason: str = "",
    updated_input: dict | None = None,
) -> dict:
    """Assemble a reply for ``request`` and have ``sign`` sign it (§7).

    Echoes ``v/session_id/tool_name/input_sha256/nonce/ts`` from the request; the
    responder contributes ``behavior/reason/updated_input``. ``updated_input`` is
    honored only on ``allow``. The signature covers the recomputed
    ``updated_input_sha256`` — the hash itself is not sent on the wire.

    ``sign`` receives the §7 signing bytes and returns the base64 signature. That
    indirection is the seam between key custodians: a local software key signs via
    :func:`lib.crypto.sign` (see :func:`build_reply`), while
    ``approver/responder_yubikey.py`` hands the same bytes to a YubiKey. The wire
    format is identical either way, so ``hook.py`` needs no knowledge of which one
    answered.
    """
    if behavior not in _BEHAVIORS:
        raise ValueError(f"behavior must be one of {_BEHAVIORS}, got {behavior!r}")

    apply_update = behavior == "allow" and updated_input is not None
    updated_input_sha256 = protocol.canonical_sha256(updated_input) if apply_update else ""

    sig = sign(
        protocol.signing_bytes(
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
    )

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


def build_reply(
    request: dict,
    *,
    behavior: str,
    key_id: str,
    private_b64: str,
    key_type: str = crypto.DEFAULT_KEY_TYPE,
    reason: str = "",
    updated_input: dict | None = None,
) -> dict:
    """Build a reply signed with a local software key (§7).

    Thin wrapper over :func:`build_signed_reply` whose signer is
    :func:`lib.crypto.sign`. ``key_type`` selects the scheme; the hook picks it up
    from the allowlist by ``key_id`` (it is not carried in the reply), so it is not
    part of the signed bytes.
    """
    return build_signed_reply(
        request,
        behavior=behavior,
        key_id=key_id,
        sign=lambda sb: crypto.sign(private_b64, sb, key_type),
        reason=reason,
        updated_input=updated_input,
    )


# --- commands ------------------------------------------------------------------
async def register(
    token: str,
    *,
    key_type: str = crypto.DEFAULT_KEY_TYPE,
    config_path: Path | str = DEFAULT_CONFIG,
    servers: str = bus.DEFAULT_SERVERS,
    timeout: float = 10.0,
) -> dict:
    """Register/rotate this responder's key. Persists only on ``ok:true``.

    ``key_type`` (``ed25519``/``p256``) picks the scheme for the freshly generated
    pair and is announced to the handler so it pins the same algorithm.

    The handler's own signature over the reply is verified before anything is
    believed or written, against the server key already in the config if there is
    one (§6). The verified key is stored alongside the pair, so every later
    registration is pinned to the same handler.

    Returns the handler's reply dict. Raises ``ValueError`` on a malformed token,
    ``bus.BusError`` on transport failure (no handler / timeout) and
    :class:`ServerReplyError` when the answer is not the handler's.
    """
    key_id = parse_key_id(token)
    keypair = crypto.generate_keypair(key_type)
    nonce = new_nonce()
    req = build_registration_request(
        token, keypair.public_b64(), int(time.time()), key_type=key_type, nonce=nonce
    )
    pinned = pinned_server_key(config_path)

    async with bus.connect(servers, name=bus.client_name("responder-register", key_id)) as b:
        reply = await b.request("registrations", req, timeout=timeout)

    server_key = verify_server_reply(reply, nonce=nonce, pinned_pubkey=pinned)

    if reply.get("ok"):
        cfg = configlib.Config(
            config_path,
            {
                "v": protocol.PROTOCOL_VERSION,
                "key_id": key_id,
                "key_type": key_type,
                "private_key": keypair.private_b64(),
                "public_key": keypair.public_b64(),
                "server_key": server_key,
            },
        )
        cfg.save()
    return reply


def print_request(request: dict) -> None:
    """Render a permission request on stderr, for an operator about to decide on it.

    Split out from :func:`prompt_operator` so other front ends can show the same
    thing while asking differently — ``responder_yubikey.py`` reuses it for a prompt
    that takes the keystroke and nothing else.
    """
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


def prompt_operator(request: dict):
    """Blocking console prompt. Returns ``(behavior, reason, updated_input)`` or None to skip."""
    print_request(request)
    answer = input("allow / deny / skip? [a/d/s]: ").strip().lower()
    if answer in ("a", "allow"):
        return ("allow", input("reason (optional): ").strip(), None)
    if answer in ("d", "deny"):
        return ("deny", input("reason (optional): ").strip(), None)
    print("skipped (no reply — hook falls back to the interactive prompt)", file=sys.stderr)
    return None


def make_approval_handler(
    *,
    key_id: str,
    sign: Callable[[bytes], str],
    prompt=prompt_operator,
    on_signed: Callable[[dict], None] | None = None,
):
    """Build the ``approvals.*`` handler: ask the operator, sign the decision, reply.

    Prompting and signing both happen in a worker thread — they block for as long
    as a human takes (and, for a YubiKey, for as long as the touch takes), and the
    event loop has to stay free for NATS heartbeats.

    Returns ``None`` (no reply published) when the operator skips **and** when
    signing fails: a signer that cannot sign — key unplugged, touch timed out — must
    neither crash the responder loop nor answer, so the hook falls back to the
    interactive prompt (§7 fail-safe).

    ``on_signed`` is handed each finished reply just before it goes on the wire, for
    front ends that want to confirm what was signed (see
    ``responder_yubikey.print_decision``). It runs guarded: reporting is a courtesy
    and must never cost a valid decision.
    """

    def decide(request: dict):
        decision = prompt(request)
        if not decision:
            return None
        behavior, reason, updated_input = decision
        try:
            reply = build_signed_reply(
                request,
                behavior=behavior,
                key_id=key_id,
                sign=sign,
                reason=reason,
                updated_input=updated_input,
            )
        except Exception as e:  # noqa: BLE001 — any signing failure → stay silent
            # ASCII only: this can surface on a Windows console with a legacy codepage.
            print(f"could not sign the decision: {e}", file=sys.stderr)
            print("no reply sent - Claude Code falls back to its own prompt", file=sys.stderr)
            return None

        if on_signed is not None:
            try:
                on_signed(reply)
            except Exception as e:  # noqa: BLE001 — never drop a signed reply over this
                print(f"could not report the signed decision: {e}", file=sys.stderr)
        return reply

    async def handler(request: dict):
        return await asyncio.to_thread(decide, request)

    return handler


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
    # Older configs predate key_type; default to Ed25519 to stay backward compatible.
    key_type = cfg.get("key_type", crypto.DEFAULT_KEY_TYPE)

    handler = make_approval_handler(
        key_id=key_id,
        sign=lambda sb: crypto.sign(private_b64, sb, key_type),
        prompt=prompt,
    )

    # The `key_id` is the identity that matters here: it is what the reply
    # carries and what the allowlist is keyed by, so `/connz` naming the same
    # thing is what answers "which of the responders in this queue group
    # answered?" (§6, "Multiple clients").
    async with bus.connect(servers, name=bus.client_name("responder", key_id)) as b:
        await b.reply(subject, handler, queue=queue)
        print(
            f"responder key_id={key_id!r} ({key_type}) serving {subject!r} "
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
    p_reg.add_argument(
        "--key-type",
        choices=crypto.KEY_TYPES,
        default=crypto.DEFAULT_KEY_TYPE,
        help="signature scheme for the generated key (default: %(default)s)",
    )
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
                    key_type=args.key_type,
                    config_path=Path(args.config),
                    servers=args.servers,
                    timeout=args.timeout,
                )
            )
        except (ValueError, bus.BusError, ServerReplyError) as e:
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
