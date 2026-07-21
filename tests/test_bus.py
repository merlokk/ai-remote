"""Tests for lib.bus — JSON request-reply over NATS.

These are integration tests against a live NATS server; they skip when it is
unreachable. Async bodies are driven with asyncio.run (pytest-asyncio is not an
approved dependency — see CLAUDE.md §1).
"""
import uuid

import pytest

from lib.bus import Bus, NoResponders, RequestTimeout, connect
from tests.conftest import requires_nats, run_async

pytestmark = requires_nats


def _subject() -> str:
    return f"test.{uuid.uuid4().hex}"


def test_request_reply_roundtrip():
    subject = _subject()

    async def body():
        async with connect() as bus:
            async def handler(req):
                return {"echo": req["ping"], "ok": True}

            await bus.reply(subject, handler)
            reply = await bus.request(subject, {"ping": "hello"}, timeout=2.0)
            return reply

    reply = run_async(body())
    assert reply == {"echo": "hello", "ok": True}


def test_request_no_responders_raises():
    async def body():
        async with connect() as bus:
            await bus.request(_subject(), {"x": 1}, timeout=2.0)

    with pytest.raises(NoResponders):
        run_async(body())


def test_request_times_out_when_responder_silent():
    subject = _subject()

    async def body():
        async with connect() as bus:
            async def silent(req):
                return None  # handler returns nothing -> no reply published

            await bus.reply(subject, silent)
            await bus.request(subject, {"x": 1}, timeout=0.3)

    with pytest.raises(RequestTimeout):
        run_async(body())


def test_sync_handler_is_supported():
    subject = _subject()

    async def body():
        async with connect() as bus:
            def handler(req):  # plain sync function, not a coroutine
                return {"doubled": req["n"] * 2}

            await bus.reply(subject, handler)
            return await bus.request(subject, {"n": 21}, timeout=2.0)

    assert run_async(body()) == {"doubled": 42}


def test_queue_group_delivers_to_single_subscriber():
    subject = _subject()

    async def body():
        async with connect() as bus:
            hits = {"a": 0, "b": 0}

            def make(name):
                def handler(req):
                    hits[name] += 1
                    return {"by": name}
                return handler

            await bus.reply(subject, make("a"), queue="workers")
            await bus.reply(subject, make("b"), queue="workers")

            for _ in range(6):
                await bus.request(subject, {"x": 1}, timeout=2.0)
            return hits

    hits = run_async(body())
    assert hits["a"] + hits["b"] == 6
    # Queue group => each message handled by exactly one subscriber, not both.
    assert hits["a"] > 0 and hits["b"] >= 0


def test_connect_yields_bus():
    async def body():
        async with connect() as bus:
            assert isinstance(bus, Bus)

    run_async(body())
