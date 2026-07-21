"""Tests for approver.registration_handler — token minting + allowlist writes (§6).

Pure logic is unit-tested; serving is covered by NATS integration tests that skip
when the broker is down (async driven via asyncio.run — no pytest-asyncio).
"""
import base64
import time
import uuid

import pytest

import asyncio

from approver import protocol, responder
from approver import registration_handler as rh
from lib.bus import NoResponders, connect
from lib.config import Config
from tests.conftest import requires_nats, run_async

V = protocol.PROTOCOL_VERSION


def _data(pending=None, clients=None):
    return {"v": V, "pending_tokens": pending or [], "clients": clients or {}}


def _pending(key_id="approver-1", token=None, expires_ts=10_000):
    return {"key_id": key_id, "token": token or f"{key_id}.SECRET", "expires_ts": expires_ts}


def _req(token="approver-1.SECRET", key_id="approver-1", pubkey="PUB=="):
    return {"v": V, "token": token, "key_id": key_id, "pubkey": pubkey, "ts": 123}


# --- handle_registration -------------------------------------------------------
def test_valid_token_registers_client_and_consumes_token():
    data = _data(pending=[_pending()])
    reply, changed = rh.handle_registration(data, _req(), now=100)
    assert reply == {"v": V, "ok": True, "key_id": "approver-1"}
    assert changed is True
    assert data["clients"]["approver-1"] == {"pubkey": "PUB==", "registered_ts": 100}
    assert data["pending_tokens"] == []


def test_unknown_token_rejected():
    data = _data()
    reply, changed = rh.handle_registration(data, _req(), now=100)
    assert reply["ok"] is False and reply["error"] == "token unknown"
    assert changed is False


def test_expired_token_rejected_and_not_consumed():
    data = _data(pending=[_pending(expires_ts=50)])
    reply, changed = rh.handle_registration(data, _req(), now=100)
    assert reply["error"] == "expired"
    assert changed is False
    assert len(data["pending_tokens"]) == 1


def test_key_id_mismatch_rejected_and_not_consumed():
    # Attacker presents approver-1's token but claims approver-2's slot.
    data = _data(pending=[_pending(key_id="approver-1", token="approver-1.SECRET")])
    reply, changed = rh.handle_registration(
        data, _req(token="approver-1.SECRET", key_id="approver-2"), now=100
    )
    assert reply["error"] == "key_id mismatch"
    assert changed is False
    assert len(data["pending_tokens"]) == 1  # someone else's token stays intact


@pytest.mark.parametrize(
    "bad",
    [
        "not-a-dict",
        {"v": V, "key_id": "approver-1", "pubkey": "P"},          # missing token
        {"v": V, "token": "approver-1.S", "pubkey": "P"},          # missing key_id
        {"v": V, "token": "approver-1.S", "key_id": "approver-1"}, # missing pubkey
        {"v": 999, "token": "approver-1.S", "key_id": "approver-1", "pubkey": "P"},  # bad v
        {"v": V, "token": "", "key_id": "approver-1", "pubkey": "P"},  # empty token
    ],
)
def test_bad_request_rejected(bad):
    data = _data(pending=[_pending()])
    reply, changed = rh.handle_registration(data, bad, now=100)
    assert reply["ok"] is False and reply["error"] == "bad request"
    assert changed is False


def test_registration_rotates_existing_key():
    data = _data(
        pending=[_pending()],
        clients={"approver-1": {"pubkey": "OLD", "registered_ts": 1}},
    )
    rh.handle_registration(data, _req(pubkey="NEW=="), now=200)
    assert data["clients"]["approver-1"] == {"pubkey": "NEW==", "registered_ts": 200}


def test_token_is_one_time():
    data = _data(pending=[_pending()])
    rh.handle_registration(data, _req(), now=100)
    reply, changed = rh.handle_registration(data, _req(), now=100)
    assert reply["error"] == "token unknown"
    assert changed is False


def test_only_matching_token_is_removed():
    data = _data(
        pending=[
            _pending(key_id="approver-1", token="approver-1.A"),
            _pending(key_id="approver-2", token="approver-2.B"),
        ]
    )
    rh.handle_registration(data, _req(token="approver-1.A", key_id="approver-1"), now=100)
    assert [t["token"] for t in data["pending_tokens"]] == ["approver-2.B"]


# --- token minting -------------------------------------------------------------
def test_add_pending_token_appends_record_and_returns_token():
    data = _data()
    token = rh.add_pending_token(data, "approver-1", now=1000, ttl=900, secret_b64="U0VD")
    assert token == "approver-1.U0VD"
    assert data["pending_tokens"] == [
        {"key_id": "approver-1", "token": "approver-1.U0VD", "expires_ts": 1900}
    ]


def test_new_secret_is_32_random_bytes():
    assert len(base64.b64decode(rh.new_secret_b64())) == 32
    assert rh.new_secret_b64() != rh.new_secret_b64()


def test_validate_key_id_rejects_dot_and_empty():
    rh.validate_key_id("approver-1")  # ok
    with pytest.raises(ValueError):
        rh.validate_key_id("has.dot")
    with pytest.raises(ValueError):
        rh.validate_key_id("")


def test_add_pending_token_rejects_bad_key_id():
    with pytest.raises(ValueError):
        rh.add_pending_token(_data(), "a.b", now=1)


def test_sweep_expired_drops_only_expired():
    data = _data(
        pending=[_pending(token="t1", expires_ts=50), _pending(token="t2", expires_ts=200)]
    )
    removed = rh.sweep_expired(data, now=100)
    assert removed == 1
    assert [t["token"] for t in data["pending_tokens"]] == ["t2"]


def test_get_token_writes_config(tmp_path):
    p = tmp_path / "handler-config.json"
    token = rh.get_token("approver-1", config_path=p, ttl=900, now=1000)
    assert token.startswith("approver-1.")
    cfg = Config.load(p)
    assert cfg["pending_tokens"][0]["token"] == token
    assert cfg["pending_tokens"][0]["expires_ts"] == 1900


# --- integration ---------------------------------------------------------------
@requires_nats
def test_handler_serves_registration_over_nats(tmp_path):
    p = tmp_path / "handler-config.json"
    subject = f"reg.{uuid.uuid4().hex}"
    now = int(time.time())
    token = rh.get_token("approver-1", config_path=p, ttl=900, now=now)
    req = {"v": V, "token": token, "key_id": "approver-1", "pubkey": "PUB==", "ts": now}

    async def body():
        async with connect() as b:
            await b.reply(subject, rh.make_handler(p))
            return await b.request(subject, req, timeout=2.0)

    reply = run_async(body())
    assert reply["ok"] is True
    cfg = Config.load(p)
    assert cfg["clients"]["approver-1"]["pubkey"] == "PUB=="
    assert cfg["pending_tokens"] == []


@requires_nats
def test_handler_reloads_tokens_minted_after_start(tmp_path):
    # Handler is created against an empty config; a token minted afterwards (by a
    # separate --get-token invocation) must be visible because each message reloads.
    p = tmp_path / "handler-config.json"
    subject = f"reg.{uuid.uuid4().hex}"

    async def body():
        async with connect() as b:
            await b.reply(subject, rh.make_handler(p))
            now = int(time.time())
            token = rh.get_token("approver-1", config_path=p, ttl=900, now=now)
            req = {"v": V, "token": token, "key_id": "approver-1", "pubkey": "P==", "ts": now}
            return await b.request(subject, req, timeout=2.0)

    assert run_async(body())["ok"] is True


@requires_nats
def test_responder_registers_against_handler_end_to_end(tmp_path):
    hcfg = tmp_path / "handler-config.json"
    rcfg = tmp_path / "responder-config.json"
    token = rh.get_token("approver-1", config_path=hcfg, ttl=900, now=int(time.time()))

    async def body():
        async with connect() as b:
            await b.reply("registrations", rh.make_handler(hcfg))
            return await responder.register(token, config_path=rcfg, timeout=2.0)

    reply = run_async(body())
    assert reply["ok"] is True
    rc = Config.load(rcfg)
    hc = Config.load(hcfg)
    # Handler recorded exactly the public key the responder kept privately.
    assert hc["clients"]["approver-1"]["pubkey"] == rc["public_key"]
    assert hc["pending_tokens"] == []


@requires_nats
def test_serve_once_exits_after_one_registration(tmp_path):
    p = tmp_path / "handler-config.json"
    subject = f"reg.{uuid.uuid4().hex}"
    now = int(time.time())
    token = rh.get_token("approver-1", config_path=p, ttl=900, now=now)
    req = {"v": V, "token": token, "key_id": "approver-1", "pubkey": "PUB==", "ts": now}

    async def body():
        server = asyncio.create_task(rh.serve(config_path=p, subject=subject, once=True))
        async with connect() as client:
            reply = None
            for _ in range(50):  # wait for the server subscription to come up
                try:
                    reply = await client.request(subject, req, timeout=1.0)
                    break
                except NoResponders:
                    await asyncio.sleep(0.1)
            # serve(once=True) must return on its own once a registration succeeds.
            await asyncio.wait_for(server, timeout=5.0)
            return reply

    reply = run_async(body())
    assert reply is not None and reply["ok"] is True
    assert Config.load(p)["clients"]["approver-1"]["pubkey"] == "PUB=="
