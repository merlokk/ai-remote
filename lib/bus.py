"""JSON request-reply over NATS — a thin async wrapper over nats-py.

Both sides of the approval protocol (CLAUDE.md §7) speak through a ``Bus``:

    async with connect() as bus:
        # requester (hook.py):
        reply = await bus.request("approvals.<sid>", payload, timeout=30)
        # responder (responder.py):
        await bus.reply("approvals.*", handler, queue="approvers")

Payloads and replies are plain dicts, encoded as compact UTF-8 JSON. NATS-level
failures surface as ``RequestTimeout`` / ``NoResponders`` so callers (e.g. the
fail-safe hook) can map every error to the interactive-prompt fallback.
"""
from __future__ import annotations

import inspect
import json
import sys
from collections.abc import Awaitable, Callable
from contextlib import asynccontextmanager
from typing import Any

import nats
from nats.aio.client import Client as NATSClient
from nats.aio.subscription import Subscription
from nats.errors import NoRespondersError
from nats.errors import TimeoutError as NatsTimeoutError

DEFAULT_SERVERS = "nats://127.0.0.1:4222"

Handler = Callable[[dict[str, Any]], dict[str, Any] | None | Awaitable[dict[str, Any] | None]]


class BusError(Exception):
    """Base class for bus-level failures."""


class RequestTimeout(BusError):
    """No reply arrived within the timeout."""


class NoResponders(BusError):
    """No subscriber was listening on the subject."""


def client_name(role: str, ident: str | None = None) -> str:
    """What this connection calls itself on the bus (``nats/CLAUDE.md`` §4).

    A bare ``role`` for a program there is only ever one of, ``role:ident``
    where a second one is normal and telling them apart is the point — the
    ``key_id`` for a responder, the session for a hook.

    Only ``/connz`` ever shows this, which is exactly why it matters: it is the
    one place an operator can look when two responders share a subject and only
    one is answering (``approver/CLAUDE.md`` §6, "Multiple clients"). An unnamed
    client shows up there as ``name: null`` and is then distinguishable only by
    its IP and port.
    """
    return f"{role}:{ident}" if ident else role


def _encode(obj: Any) -> bytes:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def _decode(data: bytes) -> Any:
    return json.loads(data.decode("utf-8"))


class Bus:
    """Request-reply helpers bound to a connected NATS client."""

    def __init__(self, nc: NATSClient):
        self._nc = nc

    @property
    def client(self) -> NATSClient:
        return self._nc

    async def request(
        self, subject: str, payload: dict[str, Any], *, timeout: float = 30.0
    ) -> dict[str, Any]:
        """Send ``payload`` and return the decoded reply.

        Raises ``NoResponders`` if nobody is subscribed, ``RequestTimeout`` if no
        reply arrives in time.
        """
        try:
            msg = await self._nc.request(subject, _encode(payload), timeout=timeout)
        except NoRespondersError as e:
            raise NoResponders(f"no responders on {subject!r}") from e
        except NatsTimeoutError as e:
            raise RequestTimeout(f"no reply on {subject!r} within {timeout}s") from e
        return _decode(msg.data)

    async def reply(
        self, subject: str, handler: Handler, *, queue: str | None = None
    ) -> Subscription:
        """Serve ``subject``: for each request, call ``handler`` and reply with its result.

        ``handler`` may be sync or async and receives the decoded request dict. If it
        returns ``None`` (or the message has no reply inbox), no reply is published.
        Pass ``queue`` to load-balance across responders via a NATS queue group.

        A message that is not a JSON object is dropped with a warning and never
        reaches ``handler``: subjects are open, so a stray ``nats pub`` / ``nats req``
        can hand a live responder anything at all, and that must not surface as a
        traceback out of the subscription callback (nor be answered).
        """

        async def cb(msg):
            try:
                req = _decode(msg.data)
            except (UnicodeDecodeError, json.JSONDecodeError) as e:
                # ASCII only: this can land on a Windows console with a legacy codepage.
                print(f"bus: could not decode a message on {msg.subject!r}: {e}", file=sys.stderr)
                return
            if not isinstance(req, dict):
                print(
                    f"bus: could not decode a message on {msg.subject!r}: "
                    f"expected a JSON object, got {type(req).__name__}",
                    file=sys.stderr,
                )
                return
            result = handler(req)
            if inspect.isawaitable(result):
                result = await result
            if result is not None and msg.reply:
                await self._nc.publish(msg.reply, _encode(result))

        return await self._nc.subscribe(subject, queue=queue or "", cb=cb)

    async def publish(self, subject: str, payload: dict[str, Any]) -> None:
        await self._nc.publish(subject, _encode(payload))

    async def flush(self) -> None:
        await self._nc.flush()


@asynccontextmanager
async def connect(
    servers: str | list[str] = DEFAULT_SERVERS,
    *,
    name: str | None = None,
    **kwargs,
):
    """Async context manager yielding a connected ``Bus``; drains on exit.

    ``name`` is what ``/connz`` will call this connection — build it with
    :func:`client_name` so every client in the repository says who it is the same
    way. It defaults to ``None`` (anonymous) so that nothing breaks by omitting
    it, but every caller in this repository names itself.
    """
    nc = await nats.connect(servers, name=name, **kwargs)
    try:
        yield Bus(nc)
    finally:
        await nc.drain()
