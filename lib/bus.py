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
        """

        async def cb(msg):
            req = _decode(msg.data)
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
async def connect(servers: str | list[str] = DEFAULT_SERVERS, **kwargs):
    """Async context manager yielding a connected ``Bus``; drains on exit."""
    nc = await nats.connect(servers, **kwargs)
    try:
        yield Bus(nc)
    finally:
        await nc.drain()
