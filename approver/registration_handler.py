"""registration_handler.py — bootstrap of trusted responder keys (CLAUDE.md §6).

(The design doc calls this ``registration-handler.py``; the module uses an
underscore so it is importable / unit-testable.)

Owner of the allowlist stored in ``handler-config.json`` (the ``clients`` map that
``hook.py`` reads). Two modes:

  --get-token <key_id>   Mint a one-time token ``<key_id>.<secret>`` (default TTL
                         15 min), record it in ``pending_tokens`` and print it to
                         stdout. Hand the token to the operator out of band.

  (serve, default)       Listen on ``registrations``. For each request: match the
                         token, verify it is unexpired and bound to the claimed
                         ``key_id``, then write ``clients[key_id]`` (rotating any
                         previous key) and consume the token. A token is spent only
                         on success. With ``--once``, exit after the first
                         successful registration instead of running forever
                         (scripted e2e runs need no process to kill).

  --new-server-key       Generate the handler's own signing key, replacing the
                         current one if there is one, print the new public half
                         and exit. Every approver pinned to the old key then has
                         to register again.

The handler has an Ed25519 key of its own, generated into ``handler-config.json``
on first use and rotated only when ``--new-server-key`` says so. **Every reply it
sends is signed with it**, acceptances and rejections alike, and each approver
pins the public half at registration - so an approver can tell the allowlist owner
apart from anyone else who can publish on the open ``registrations`` subject.

Run with the `py` launcher (CLAUDE.md §5):
  py approver/registration_handler.py --get-token approver-1
  py approver/registration_handler.py [--once]
"""
from __future__ import annotations

import argparse
import asyncio
import base64
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

DEFAULT_CONFIG = Path(__file__).resolve().parent / "handler-config.json"
DEFAULT_SUBJECT = "registrations"
DEFAULT_TTL = 15 * 60  # seconds
_SECRET_BYTES = 32


def _empty_config() -> dict:
    return {"v": protocol.PROTOCOL_VERSION, "pending_tokens": [], "clients": {}}


def _load_data(config_path: Path | str) -> dict:
    return configlib.Config.load(config_path, default=_empty_config()).data


def _save_data(config_path: Path | str, data: dict) -> None:
    configlib.Config(config_path, data).save()


# --- the server's own key ------------------------------------------------------
# The handler signs everything it says, so an approver can tell an answer from the
# allowlist owner apart from an answer by anyone else who happens to be on the bus
# (`registrations` is an open subject). The key is generated on first use and then
# never rotates on its own: approvers pin the public half at registration, and a
# silent rotation would lock every one of them out.
def _server_key_pair(record: object) -> tuple[str, str]:
    """Validate a stored ``server_key`` record; return ``(private_b64, public_b64)``.

    Raises ``ConfigError`` rather than regenerating: a corrupt record means the
    identity approvers pinned is gone, and quietly minting a new one would turn
    that into a mystery instead of a message.
    """
    if not isinstance(record, dict):
        raise configlib.ConfigError("'server_key' must be a JSON object")

    key_type = record.get("key_type", protocol.SERVER_KEY_TYPE)
    if key_type != protocol.SERVER_KEY_TYPE:
        raise configlib.ConfigError(
            f"'server_key' has key_type {key_type!r}, expected {protocol.SERVER_KEY_TYPE!r}"
        )

    private_b64, public_b64 = record.get("private_key"), record.get("public_key")
    if not isinstance(private_b64, str) or not isinstance(public_b64, str):
        raise configlib.ConfigError("'server_key' needs both 'private_key' and 'public_key'")

    try:
        derived = crypto.KeyPair.from_private_b64(private_b64, key_type).public_b64()
    except Exception as e:  # noqa: BLE001 - bad base64 / wrong length / anything else
        raise configlib.ConfigError(f"'server_key' private key is unusable: {e}") from e
    if derived != public_b64:
        raise configlib.ConfigError(
            "'server_key' public_key is not the one its private_key produces - the config "
            "has been edited or corrupted; approvers pinned the published half"
        )
    return private_b64, public_b64


def _new_server_key_record() -> dict:
    keypair = crypto.generate_keypair(protocol.SERVER_KEY_TYPE)
    return {
        "key_type": protocol.SERVER_KEY_TYPE,
        "private_key": keypair.private_b64(),
        "public_key": keypair.public_b64(),
    }


def ensure_server_key(data: dict) -> bool:
    """Give ``data`` a server signing key if it has none. True iff one was generated.

    The caller persists ``data`` when this returns True.
    """
    record = data.get("server_key")
    if record is None:
        data["server_key"] = _new_server_key_record()
        return True
    _server_key_pair(record)  # validate an existing one, never replace it
    return False


def regenerate_server_key(data: dict) -> tuple[str, str | None]:
    """Replace ``data``'s server key with a fresh pair, whatever was there before.

    Returns ``(new_public_b64, previous_public_b64 or None)``. The only path that
    discards a working key, so it is never reached by accident: ``ensure_server_key``
    keeps what it finds, and this runs only when the operator asks for it
    (``--new-server-key``). It is also the repair for a corrupt record, which is why
    it does not validate what it overwrites.

    Every approver holding the old key must register again - a rotation invalidates
    the pin, by design.
    """
    previous = data.get("server_key")
    previous_public = previous.get("public_key") if isinstance(previous, dict) else None
    data["server_key"] = _new_server_key_record()
    return data["server_key"]["public_key"], previous_public


def load_server_key(config_path: Path | str) -> str:
    """Ensure the handler has a signing key on disk; return its public half."""
    data = _load_data(config_path)
    if ensure_server_key(data):
        _save_data(config_path, data)
    return data["server_key"]["public_key"]


def rotate_server_key(config_path: Path | str) -> tuple[str, str | None]:
    """Generate a fresh server key into the config; return ``(new, previous)``.

    Touches nothing else: the ``clients`` allowlist and any live ``pending_tokens``
    are about responder keys, and a new handler identity says nothing about those.
    """
    data = _load_data(config_path)
    new_public, previous_public = regenerate_server_key(data)
    _save_data(config_path, data)
    return new_public, previous_public


def request_nonce(request: object) -> str:
    """The nonce to echo back, or ``""`` — junk on an open subject must still sign."""
    if isinstance(request, dict):
        nonce = request.get("nonce")
        if isinstance(nonce, str):
            return nonce
    return ""


def sign_reply(data: dict, reply: dict, request: object, now: int) -> dict:
    """Stamp a reply with the echoed nonce, ``ts``, the server key and its signature.

    Every reply is signed, rejections included: "token unknown" is exactly the
    answer an attacker would want to forge (or suppress and replace), and an
    approver that only checked acceptances would still act on a lie.
    """
    private_b64, public_b64 = _server_key_pair(data.get("server_key"))
    signed = dict(reply)
    signed["nonce"] = request_nonce(request)
    signed["ts"] = now
    signed["server_key"] = public_b64
    signed["sig"] = crypto.sign(
        private_b64,
        protocol.registration_reply_signing_bytes(
            v=signed.get("v", protocol.PROTOCOL_VERSION),
            ok=bool(signed.get("ok")),
            key_id=signed.get("key_id", ""),
            nonce=signed["nonce"],
            ts=now,
            error=signed.get("error", ""),
        ),
        protocol.SERVER_KEY_TYPE,
    )
    return signed


# --- token minting -------------------------------------------------------------
def validate_key_id(key_id: str) -> None:
    """A key_id must be non-empty and contain no '.' (the token separator, §6)."""
    if not key_id or "." in key_id:
        raise ValueError("key_id must be non-empty and contain no '.'")


def new_secret_b64() -> str:
    return base64.b64encode(secrets.token_bytes(_SECRET_BYTES)).decode("ascii")


def add_pending_token(
    data: dict, key_id: str, now: int, *, ttl: int = DEFAULT_TTL, secret_b64: str | None = None
) -> str:
    """Append a ``{key_id, token, expires_ts}`` record and return the token string."""
    validate_key_id(key_id)
    secret = secret_b64 or new_secret_b64()
    token = f"{key_id}.{secret}"
    data.setdefault("pending_tokens", []).append(
        {"key_id": key_id, "token": token, "expires_ts": now + ttl}
    )
    return token


def sweep_expired(data: dict, now: int) -> int:
    """Drop expired pending tokens; return how many were removed."""
    pending = data.get("pending_tokens", [])
    kept = [t for t in pending if t.get("expires_ts", 0) > now]
    data["pending_tokens"] = kept
    return len(pending) - len(kept)


def get_token(
    key_id: str, *, config_path: Path | str = DEFAULT_CONFIG, ttl: int = DEFAULT_TTL, now: int
) -> str:
    """Mint a token for ``key_id``, persist it, and return it."""
    validate_key_id(key_id)
    data = _load_data(config_path)
    ensure_server_key(data)  # minting is the handler's first run as often as not
    sweep_expired(data, now)
    token = add_pending_token(data, key_id, now, ttl=ttl)
    _save_data(config_path, data)
    return token


# --- registration handling -----------------------------------------------------
def _error(msg: str) -> dict:
    return {"v": protocol.PROTOCOL_VERSION, "ok": False, "error": msg}


def _valid_request(request) -> bool:
    if not isinstance(request, dict):
        return False
    if request.get("v") != protocol.PROTOCOL_VERSION:
        return False
    for field in ("token", "key_id", "pubkey"):
        value = request.get(field)
        if not isinstance(value, str) or not value:
            return False
    # key_type is optional (older responders omit it → Ed25519), but if present it
    # must name a scheme the hook can actually verify with.
    if "key_type" in request and request["key_type"] not in crypto.KEY_TYPES:
        return False
    return True


def handle_registration(data: dict, request, now: int) -> tuple[dict, bool]:
    """Process one registration request against ``data`` (mutated in place on success).

    Returns ``(reply, changed)`` where ``changed`` is True iff ``data`` was modified
    (and therefore must be persisted). The token is consumed only on success, so any
    rejection leaves ``pending_tokens`` untouched.
    """
    if not _valid_request(request):
        return _error("bad request"), False

    token = request["token"]
    key_id = request["key_id"]
    pubkey = request["pubkey"]
    # Absent key_type means a pre-key_type responder → Ed25519 (backward compatible).
    key_type = request.get("key_type", crypto.DEFAULT_KEY_TYPE)

    # The token is bound to the key_id in its prefix — you can only register your slot.
    if key_id != token.split(".", 1)[0]:
        return _error("key_id mismatch"), False

    record = next(
        (t for t in data.get("pending_tokens", []) if t.get("token") == token), None
    )
    if record is None:
        return _error("token unknown"), False
    if record.get("key_id") != key_id:
        return _error("key_id mismatch"), False
    if now >= record.get("expires_ts", 0):
        return _error("expired"), False

    data.setdefault("clients", {})[key_id] = {
        "pubkey": pubkey,
        "key_type": key_type,
        "registered_ts": now,
    }
    data["pending_tokens"] = [t for t in data["pending_tokens"] if t.get("token") != token]
    return {"v": protocol.PROTOCOL_VERSION, "ok": True, "key_id": key_id}, True


def make_handler(config_path: Path | str, *, lock: asyncio.Lock | None = None):
    """Build the ``registrations`` message handler.

    Reloads the config from disk per message so tokens minted by concurrent
    ``--get-token`` invocations are seen, and serializes the read-modify-write with
    a lock so overlapping registrations in this process cannot lose updates.
    """
    lock = lock or asyncio.Lock()

    async def handler(request):
        async with lock:
            now = int(time.time())
            data = _load_data(config_path)
            minted = ensure_server_key(data)  # e.g. the config was deleted under us
            reply, changed = handle_registration(data, request, now)
            if minted or changed:
                _save_data(config_path, data)
            return sign_reply(data, reply, request, now)

    return handler


async def serve(
    *,
    config_path: Path | str = DEFAULT_CONFIG,
    servers: str = bus.DEFAULT_SERVERS,
    subject: str = DEFAULT_SUBJECT,
    once: bool = False,
) -> None:
    """Listen for registrations. Runs until interrupted, or until the first
    successful registration when ``once`` is set (useful for scripted e2e runs)."""
    # Fails fast on an unreadable / wrong-version config, and mints the signing key
    # on a first run so the identity approvers pin exists before anyone can ask.
    server_key = load_server_key(config_path)
    base_handler = make_handler(config_path)
    stop = asyncio.Event()

    async def handler(request):
        reply = await base_handler(request)
        if once and reply.get("ok"):
            stop.set()
        return reply

    # No identity appended: there is one allowlist owner, and two of these on a
    # bus is a misconfiguration rather than a thing to tell apart (§6).
    async with bus.connect(servers, name=bus.client_name("registration-handler")) as b:
        await b.reply(subject, handler)
        mode = " (once)" if once else ""
        print(
            f"registration handler serving {subject!r}{mode} "
            f"(config: {config_path}) — Ctrl+C to stop",
            file=sys.stderr,
        )
        # Printed so it can be compared out of band with what an approver pinned.
        print(f"server key ({protocol.SERVER_KEY_TYPE}): {server_key}", file=sys.stderr)
        await stop.wait()  # never set unless once → same as run-forever otherwise
        if once:
            await b.flush()  # ensure the final reply is on the wire before draining


# --- CLI -----------------------------------------------------------------------
def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="registration_handler.py", description=__doc__)
    parser.add_argument(
        "--get-token",
        metavar="KEY_ID",
        help="mint a one-time token for KEY_ID and print it (otherwise: serve)",
    )
    parser.add_argument(
        "--new-server-key",
        action="store_true",
        help=(
            "generate the handler's own signing key - replacing the current one if "
            "there is one - print the new public half and exit. Every approver has "
            "to register again afterwards"
        ),
    )
    parser.add_argument("--config", default=str(DEFAULT_CONFIG))
    parser.add_argument("--servers", default=bus.DEFAULT_SERVERS)
    parser.add_argument("--ttl", type=int, default=DEFAULT_TTL, help="token TTL in seconds")
    parser.add_argument(
        "--once",
        action="store_true",
        help="serve mode: exit after the first successful registration",
    )
    return parser


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)

    if args.new_server_key:
        try:
            data = _load_data(Path(args.config))
            new_public, previous = regenerate_server_key(data)
            clients = sorted(data.get("clients", {}))
            _save_data(Path(args.config), data)
        except configlib.ConfigError as e:
            print(f"cannot write the server key: {e}", file=sys.stderr)
            return 1

        print(new_public)  # stdout = the key only, so it can be piped
        print(
            f"new server key ({protocol.SERVER_KEY_TYPE}) in {args.config}", file=sys.stderr
        )
        if previous:
            print(f"replaced {previous}", file=sys.stderr)
            # The pin is what makes rotation cost something; say so plainly rather
            # than letting it surface as an unexplained registration failure.
            print(
                "every approver pinned to the old key must register again with a fresh "
                "token; until then its 'register' fails with a key-mismatch error",
                file=sys.stderr,
            )
            if clients:
                print(f"registered approvers: {', '.join(clients)}", file=sys.stderr)
        return 0

    if args.get_token is not None:
        try:
            token = get_token(
                args.get_token, config_path=Path(args.config), ttl=args.ttl, now=int(time.time())
            )
            server_key = load_server_key(Path(args.config))
        except (ValueError, configlib.ConfigError) as e:
            print(f"cannot mint token: {e}", file=sys.stderr)
            return 1
        print(token)  # stdout = token only, so it can be piped
        print(f"expires in {args.ttl}s (config: {args.config})", file=sys.stderr)
        # The approver pins this at registration; hand it over with the token if the
        # two travel by different routes.
        print(f"server key ({protocol.SERVER_KEY_TYPE}): {server_key}", file=sys.stderr)
        return 0

    try:
        asyncio.run(
            serve(config_path=Path(args.config), servers=args.servers, once=args.once)
        )
    except KeyboardInterrupt:
        return 0
    except (configlib.ConfigError, bus.BusError) as e:
        print(f"serve failed: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
