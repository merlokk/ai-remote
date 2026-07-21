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
                         on success.

Run with the `py` launcher (CLAUDE.md §5):
  py approver/registration_handler.py --get-token approver-1
  py approver/registration_handler.py
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

    data.setdefault("clients", {})[key_id] = {"pubkey": pubkey, "registered_ts": now}
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
            data = _load_data(config_path)
            reply, changed = handle_registration(data, request, int(time.time()))
            if changed:
                _save_data(config_path, data)
            return reply

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
    _load_data(config_path)  # fail fast on an unreadable / wrong-version config
    base_handler = make_handler(config_path)
    stop = asyncio.Event()

    async def handler(request):
        reply = await base_handler(request)
        if once and reply.get("ok"):
            stop.set()
        return reply

    async with bus.connect(servers) as b:
        await b.reply(subject, handler)
        mode = " (once)" if once else ""
        print(
            f"registration handler serving {subject!r}{mode} "
            f"(config: {config_path}) — Ctrl+C to stop",
            file=sys.stderr,
        )
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

    if args.get_token is not None:
        try:
            token = get_token(
                args.get_token, config_path=Path(args.config), ttl=args.ttl, now=int(time.time())
            )
        except ValueError as e:
            print(f"cannot mint token: {e}", file=sys.stderr)
            return 1
        print(token)  # stdout = token only, so it can be piped
        print(f"expires in {args.ttl}s (config: {args.config})", file=sys.stderr)
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
