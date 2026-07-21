"""Tests for approver.responder — registration + signed approval replies (§6/§7).

Pure logic is unit-tested; the register round-trip is an integration test that
skips when NATS is down (async driven via asyncio.run — no pytest-asyncio).
"""
import uuid

import pytest

from approver import protocol, responder
from lib import crypto
from lib.bus import connect
from lib.config import Config
from tests.conftest import requires_nats, run_async


def _request(**overrides):
    req = {
        "v": protocol.PROTOCOL_VERSION,
        "session_id": "abc123",
        "tool_name": "Bash",
        "tool_input": {"command": "rm -rf build"},
        "input_sha256": protocol.canonical_sha256({"command": "rm -rf build"}),
        "permission_mode": "default",
        "cwd": "E:\\projects\\ai-remote",
        "nonce": "bm9uY2U=",
        "ts": 1737345600,
    }
    req.update(overrides)
    return req


def _hook_verifies(request, reply, pubkey_b64):
    """Mirror the hook-side signature check (§7): recompute signing bytes, verify."""
    updated = reply.get("updated_input")
    uih = protocol.canonical_sha256(updated) if updated is not None else ""
    sb = protocol.signing_bytes(
        v=request["v"],
        session_id=request["session_id"],
        nonce=request["nonce"],
        tool_name=request["tool_name"],
        input_sha256=request["input_sha256"],
        behavior=reply["behavior"],
        updated_input_sha256=uih,
        ts=request["ts"],
        reason=reply["reason"],
    )
    return crypto.verify(pubkey_b64, sb, reply["sig"])


# --- token parsing -------------------------------------------------------------
def test_parse_key_id_extracts_prefix():
    assert responder.parse_key_id("approver-1.c2VjcmV0") == "approver-1"


def test_parse_key_id_requires_dot():
    with pytest.raises(ValueError):
        responder.parse_key_id("no-dot-here")


def test_parse_key_id_rejects_empty_key_id():
    with pytest.raises(ValueError):
        responder.parse_key_id(".onlysecret")


def test_build_registration_request_shape():
    req = responder.build_registration_request("approver-1.SEKRIT", "PUB==", ts=111)
    assert req == {
        "v": protocol.PROTOCOL_VERSION,
        "token": "approver-1.SEKRIT",
        "key_id": "approver-1",
        "pubkey": "PUB==",
        "ts": 111,
    }


# --- reply building / signing --------------------------------------------------
def test_build_reply_echoes_request_fields():
    kp = crypto.generate_keypair()
    req = _request()
    reply = responder.build_reply(
        req, behavior="deny", key_id="approver-1", private_b64=kp.private_b64(), reason="no"
    )
    for f in ("v", "session_id", "tool_name", "input_sha256", "nonce", "ts"):
        assert reply[f] == req[f]
    assert reply["behavior"] == "deny"
    assert reply["reason"] == "no"
    assert reply["key_id"] == "approver-1"


def test_build_reply_signature_verifies():
    kp = crypto.generate_keypair()
    req = _request()
    reply = responder.build_reply(
        req, behavior="allow", key_id="approver-1", private_b64=kp.private_b64(), reason="ok"
    )
    assert _hook_verifies(req, reply, kp.public_b64())


def test_build_reply_allow_with_updated_input_is_signed():
    kp = crypto.generate_keypair()
    req = _request()
    reply = responder.build_reply(
        req,
        behavior="allow",
        key_id="approver-1",
        private_b64=kp.private_b64(),
        reason="rewritten",
        updated_input={"command": "npm ci"},
    )
    assert reply["updated_input"] == {"command": "npm ci"}
    assert _hook_verifies(req, reply, kp.public_b64())


def test_build_reply_deny_drops_updated_input():
    kp = crypto.generate_keypair()
    req = _request()
    reply = responder.build_reply(
        req,
        behavior="deny",
        key_id="approver-1",
        private_b64=kp.private_b64(),
        updated_input={"command": "npm ci"},
    )
    # updated_input applies only on allow (§7).
    assert "updated_input" not in reply
    assert _hook_verifies(req, reply, kp.public_b64())


def test_build_reply_rejects_bad_behavior():
    kp = crypto.generate_keypair()
    with pytest.raises(ValueError):
        responder.build_reply(
            _request(), behavior="maybe", key_id="k", private_b64=kp.private_b64()
        )


def test_build_reply_tamper_breaks_verification():
    kp = crypto.generate_keypair()
    req = _request()
    reply = responder.build_reply(
        req, behavior="allow", key_id="approver-1", private_b64=kp.private_b64(), reason="ok"
    )
    reply["behavior"] = "deny"  # attacker flips the decision after signing
    assert _hook_verifies(req, reply, kp.public_b64()) is False


# --- integration ---------------------------------------------------------------
@requires_nats
def test_register_persists_key_on_ack(tmp_path):
    cfg_path = tmp_path / "responder-config.json"
    token = f"approver-1.{uuid.uuid4().hex}"
    seen = {}

    async def body():
        async with connect() as handler_bus:
            async def handler(req):
                seen.update(req)
                return {"v": protocol.PROTOCOL_VERSION, "ok": True, "key_id": req["key_id"]}

            await handler_bus.reply("registrations", handler)
            return await responder.register(token, config_path=cfg_path, timeout=2.0)

    reply = run_async(body())
    assert reply["ok"] is True
    assert seen["key_id"] == "approver-1"

    cfg = Config.load(cfg_path)
    assert cfg["key_id"] == "approver-1"
    assert cfg["public_key"] == seen["pubkey"]
    # Persisted private key matches the registered public key.
    assert crypto.KeyPair.from_private_b64(cfg["private_key"]).public_b64() == seen["pubkey"]


@requires_nats
def test_register_does_not_persist_on_rejection(tmp_path):
    cfg_path = tmp_path / "responder-config.json"
    token = f"approver-1.{uuid.uuid4().hex}"

    async def body():
        async with connect() as handler_bus:
            async def handler(req):
                return {"v": protocol.PROTOCOL_VERSION, "ok": False, "error": "expired"}

            await handler_bus.reply("registrations", handler)
            return await responder.register(token, config_path=cfg_path, timeout=2.0)

    reply = run_async(body())
    assert reply["ok"] is False
    assert not cfg_path.exists()  # a rejected registration must not clobber config


@requires_nats
def test_signed_reply_survives_round_trip_over_nats():
    kp = crypto.generate_keypair()
    subject = f"approvals.{uuid.uuid4().hex}"
    req = _request(session_id=subject.split(".", 1)[1])

    async def body():
        async with connect() as bus:
            async def handler(request):
                return responder.build_reply(
                    request,
                    behavior="allow",
                    key_id="approver-1",
                    private_b64=kp.private_b64(),
                    reason="ok",
                )

            await bus.reply(subject, handler)
            return await bus.request(subject, req, timeout=2.0)

    reply = run_async(body())
    assert reply["behavior"] == "allow"
    assert _hook_verifies(req, reply, kp.public_b64())
