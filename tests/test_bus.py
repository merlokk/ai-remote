"""Tests for lib.bus — JSON request-reply over NATS.

These are integration tests against a live NATS server; they skip when it is
unreachable. Async bodies are driven with asyncio.run (pytest-asyncio is not an
approved dependency — see CLAUDE.md §1).
"""
import uuid

import pytest

from lib.bus import Bus, NoResponders, RequestTimeout, client_name, connect
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


def test_junk_payload_is_ignored_and_does_not_reach_the_handler(capsys):
    """Anything can be published on our subject; a non-JSON message must not blow up.

    `nats req approvals.<sid> "x"` from a console is enough to feed a live responder
    a payload that is not JSON. Before this was guarded it surfaced as a
    JSONDecodeError traceback out of the subscription callback.
    """
    subject = _subject()
    seen = []

    async def body():
        async with connect() as bus:
            await bus.reply(subject, seen.append)
            # Raw bytes: not our encoder, so this is exactly what a stray CLI sends.
            await bus.client.publish(subject, b"x", reply=bus.client.new_inbox())
            await bus.flush()
            with pytest.raises(RequestTimeout):  # ...and a real request still works
                await bus.request(subject, {"ok": 1}, timeout=0.5)

    run_async(body())

    # The junk never became a request; the valid one did.
    assert seen == [{"ok": 1}]
    assert "could not decode" in capsys.readouterr().err


def test_json_that_is_not_an_object_is_ignored():
    # Valid JSON, wrong shape: handlers are documented to receive a dict, and
    # `5`.get(...) would raise inside the callback just like bad JSON did.
    subject = _subject()
    seen = []

    async def body():
        async with connect() as bus:
            await bus.reply(subject, seen.append)
            await bus.client.publish(subject, b"5", reply=bus.client.new_inbox())
            await bus.flush()
            with pytest.raises(RequestTimeout):
                await bus.request(subject, {"ok": 1}, timeout=0.5)

    run_async(body())

    assert seen == [{"ok": 1}]


def test_connect_yields_bus():
    async def body():
        async with connect() as bus:
            assert isinstance(bus, Bus)

    run_async(body())


# --- who a connection says it is (CLAUDE.md §4, "Naming a connection") --------
def test_client_name_is_the_role_or_the_role_and_one_identity():
    """A bare role for a process there is only one of, `role:identity` otherwise.

    The identity is the thing an operator needs when two clients are on one
    subject and only one is answering: the `key_id` for a responder, the session
    for a hook. An empty or absent one collapses to the bare role rather than
    leaving a dangling colon.
    """
    assert client_name("registration-handler") == "registration-handler"
    assert client_name("responder", "approver-1") == "responder:approver-1"
    assert client_name("hook", "sess-01") == "hook:sess-01"
    assert client_name("test-request", None) == "test-request"
    assert client_name("test-request", "") == "test-request"


def _connz_names() -> list[str]:
    """Every connection name the server currently reports, or skip.

    `/connz` on the monitoring port (§3) is the only place a client's name is
    observable — which is the whole reason to send one, so this test reads it
    from there rather than trusting the client's own record of what it sent.
    """
    import json
    import urllib.error
    import urllib.request

    try:
        with urllib.request.urlopen("http://127.0.0.1:8222/connz?limit=1024", timeout=2) as r:
            data = json.load(r)
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        pytest.skip(f"the NATS monitoring port is not reachable: {e}")
    return [c.get("name") for c in data.get("connections", [])]


def test_the_name_reaches_the_server_and_shows_up_in_connz():
    """The §2.8 property, from the server's side.

    Nothing in the protocol echoes a connection name back, so an operator's only
    view of it is `/connz` — and an unnamed client is exactly the thing that made
    two responders on one subject indistinguishable there.
    """
    name = client_name("test-bus", uuid.uuid4().hex[:8])

    async def body():
        async with connect(name=name) as bus:
            # Round-trip anything at all: the connection has to be established
            # (and the CONNECT flushed) before the server can report it.
            await bus.flush()
            return _connz_names()

    assert name in run_async(body())
