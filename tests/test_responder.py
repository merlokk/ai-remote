"""Tests for approver.responder — registration + signed approval replies (§6/§7).

Pure logic is unit-tested; the register round-trip is an integration test that
skips when NATS is down (async driven via asyncio.run — no pytest-asyncio).
"""
import base64
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


def _hook_verifies(request, reply, pubkey_b64, key_type=crypto.DEFAULT_KEY_TYPE):
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
    return crypto.verify(pubkey_b64, sb, reply["sig"], key_type)


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
    req = responder.build_registration_request(
        "approver-1.SEKRIT", "PUB==", ts=111, nonce="bm9uY2U="
    )
    assert req == {
        "v": protocol.PROTOCOL_VERSION,
        "token": "approver-1.SEKRIT",
        "key_id": "approver-1",
        "pubkey": "PUB==",
        "key_type": crypto.ED25519,  # default scheme
        "nonce": "bm9uY2U=",
        "ts": 111,
    }


def test_build_registration_request_carries_key_type():
    req = responder.build_registration_request(
        "approver-1.SEKRIT", "PUB==", ts=111, key_type=crypto.P256
    )
    assert req["key_type"] == crypto.P256


def test_new_nonce_is_32_random_bytes():
    assert len(base64.b64decode(responder.new_nonce())) == 32
    assert responder.new_nonce() != responder.new_nonce()


# --- verifying the handler's own signature (§6 server key) ----------------------
def _server_reply(kp, *, ok=True, key_id="approver-1", nonce="bm9uY2U=", ts=500, error=""):
    """A registration reply signed the way ``registration_handler`` signs one."""
    reply = {
        "v": protocol.PROTOCOL_VERSION,
        "ok": ok,
        "nonce": nonce,
        "ts": ts,
        "server_key": kp.public_b64(),
    }
    if ok:
        reply["key_id"] = key_id
    else:
        reply["error"] = error
    reply["sig"] = kp.sign(
        protocol.registration_reply_signing_bytes(
            v=protocol.PROTOCOL_VERSION,
            ok=ok,
            key_id=key_id if ok else "",
            nonce=nonce,
            ts=ts,
            error=error if not ok else "",
        )
    )
    return reply


def test_verify_server_reply_returns_the_signing_key():
    kp = crypto.generate_keypair()
    reply = _server_reply(kp)
    assert responder.verify_server_reply(reply, nonce="bm9uY2U=") == kp.public_b64()


def test_verify_server_reply_accepts_a_signed_rejection():
    kp = crypto.generate_keypair()
    reply = _server_reply(kp, ok=False, error="expired")
    assert responder.verify_server_reply(reply, nonce="bm9uY2U=") == kp.public_b64()


def test_verify_server_reply_rejects_a_tampered_verdict():
    kp = crypto.generate_keypair()
    reply = _server_reply(kp, ok=False, error="expired")
    reply["ok"] = True  # an attacker turns a rejection into an acceptance
    with pytest.raises(responder.ServerReplyError):
        responder.verify_server_reply(reply, nonce="bm9uY2U=")


def test_verify_server_reply_rejects_a_replayed_nonce():
    kp = crypto.generate_keypair()
    reply = _server_reply(kp, nonce="b2xkZXI=")  # signed, but for another exchange
    with pytest.raises(responder.ServerReplyError):
        responder.verify_server_reply(reply, nonce="bm9uY2U=")


def test_verify_server_reply_rejects_an_unpinned_key():
    """A valid signature by the wrong key is the attack this pinning exists for."""
    other = crypto.generate_keypair()
    reply = _server_reply(other)
    with pytest.raises(responder.ServerReplyError):
        responder.verify_server_reply(
            reply, nonce="bm9uY2U=", pinned_pubkey=crypto.generate_keypair().public_b64()
        )


def test_verify_server_reply_accepts_the_pinned_key():
    kp = crypto.generate_keypair()
    reply = _server_reply(kp)
    assert (
        responder.verify_server_reply(reply, nonce="bm9uY2U=", pinned_pubkey=kp.public_b64())
        == kp.public_b64()
    )


@pytest.mark.parametrize(
    "mutate",
    [
        lambda r: r.pop("sig"),
        lambda r: r.pop("server_key"),
        lambda r: r.pop("ts"),
        lambda r: r.update(sig="bm90LWEtc2ln"),
        lambda r: r.update(v=999),
        lambda r: r.update(ok="yes"),
        lambda r: r.update(key_id=7),
        lambda r: r.update(server_key=""),
    ],
)
def test_verify_server_reply_rejects_malformed_replies(mutate):
    kp = crypto.generate_keypair()
    reply = _server_reply(kp)
    mutate(reply)
    with pytest.raises(responder.ServerReplyError):
        responder.verify_server_reply(reply, nonce="bm9uY2U=")


def test_verify_server_reply_rejects_a_non_object():
    with pytest.raises(responder.ServerReplyError):
        responder.verify_server_reply("nope", nonce="bm9uY2U=")


def test_pinned_server_key_reads_the_config(tmp_path):
    p = tmp_path / "responder-config.json"
    assert responder.pinned_server_key(p) is None  # nothing registered yet
    Config(p, {"v": protocol.PROTOCOL_VERSION, "server_key": "SRV=="}).save()
    assert responder.pinned_server_key(p) == "SRV=="


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


def test_build_reply_signature_verifies_with_p256():
    kp = crypto.generate_keypair(crypto.P256)
    req = _request()
    reply = responder.build_reply(
        req,
        behavior="allow",
        key_id="approver-1",
        private_b64=kp.private_b64(),
        key_type=crypto.P256,
        reason="ok",
    )
    assert _hook_verifies(req, reply, kp.public_b64(), crypto.P256)
    # And a P-256 reply must not verify if checked as Ed25519.
    assert not _hook_verifies(req, reply, kp.public_b64(), crypto.ED25519)


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


def test_build_signed_reply_hands_the_exact_signing_bytes_to_the_signer():
    """The seam the YubiKey responder plugs into: a signer over the §7 bytes."""
    seen = {}

    def sign(message: bytes) -> str:
        seen["message"] = message
        return "c2lnbmF0dXJl"

    req = _request()
    reply = responder.build_signed_reply(
        req, behavior="allow", key_id="approver-1", sign=sign, reason="ok"
    )

    assert reply["sig"] == "c2lnbmF0dXJl"
    assert seen["message"] == protocol.signing_bytes(
        v=req["v"],
        session_id=req["session_id"],
        nonce=req["nonce"],
        tool_name=req["tool_name"],
        input_sha256=req["input_sha256"],
        behavior="allow",
        updated_input_sha256="",
        ts=req["ts"],
        reason="ok",
    )


def test_build_signed_reply_covers_updated_input():
    seen = {}

    def sign(message: bytes) -> str:
        seen["message"] = message
        return ""

    responder.build_signed_reply(
        _request(),
        behavior="allow",
        key_id="k",
        sign=sign,
        updated_input={"command": "npm ci"},
    )
    # The hash of updated_input sits in the signed bytes, not on the wire (§7).
    assert protocol.canonical_sha256({"command": "npm ci"}).encode() in seen["message"]


# --- approval handler ----------------------------------------------------------
def test_make_approval_handler_signs_the_operator_decision():
    kp = crypto.generate_keypair()
    req = _request()
    handler = responder.make_approval_handler(
        key_id="approver-1",
        sign=lambda sb: crypto.sign(kp.private_b64(), sb),
        prompt=lambda request: ("allow", "ok", None),
    )

    reply = run_async(handler(req))

    assert reply["behavior"] == "allow"
    assert _hook_verifies(req, reply, kp.public_b64())


def test_make_approval_handler_returns_none_when_the_operator_skips():
    handler = responder.make_approval_handler(
        key_id="k", sign=lambda sb: "unused", prompt=lambda request: None
    )
    # No reply published — the hook times out and falls back to the prompt (§7).
    assert run_async(handler(_request())) is None


def test_make_approval_handler_returns_none_when_signing_fails(capsys):
    def sign(sb):
        raise RuntimeError("device unplugged")

    handler = responder.make_approval_handler(
        key_id="k", sign=sign, prompt=lambda request: ("allow", "", None)
    )

    # Fail-safe: a signer that cannot sign must not crash the responder loop, and
    # must not answer either — silence sends the hook back to the normal prompt.
    assert run_async(handler(_request())) is None
    assert "device unplugged" in capsys.readouterr().err


def test_make_approval_handler_reports_the_signed_reply():
    kp = crypto.generate_keypair()
    seen = []
    handler = responder.make_approval_handler(
        key_id="approver-1",
        sign=lambda sb: crypto.sign(kp.private_b64(), sb),
        prompt=lambda request: ("allow", "", None),
        on_signed=seen.append,
    )

    reply = run_async(handler(_request()))

    assert seen == [reply]  # the callback sees exactly what goes on the wire


def test_make_approval_handler_still_replies_when_reporting_fails(capsys):
    # Announcing the decision is a courtesy to the operator; losing a signed reply
    # over a broken console would send Claude Code back to its own prompt.
    kp = crypto.generate_keypair()
    req = _request()
    handler = responder.make_approval_handler(
        key_id="approver-1",
        sign=lambda sb: crypto.sign(kp.private_b64(), sb),
        prompt=lambda request: ("allow", "", None),
        on_signed=lambda reply: (_ for _ in ()).throw(RuntimeError("no console")),
    )

    reply = run_async(handler(req))

    assert reply is not None and _hook_verifies(req, reply, kp.public_b64())
    assert "no console" in capsys.readouterr().err


def test_build_reply_tamper_breaks_verification():
    kp = crypto.generate_keypair()
    req = _request()
    reply = responder.build_reply(
        req, behavior="allow", key_id="approver-1", private_b64=kp.private_b64(), reason="ok"
    )
    reply["behavior"] = "deny"  # attacker flips the decision after signing
    assert _hook_verifies(req, reply, kp.public_b64()) is False


# --- integration ---------------------------------------------------------------
def _fake_handler(kp, *, ok=True, error="", seen=None, key_override=None):
    """A stand-in registration handler that signs like the real one (§6)."""

    async def handler(req):
        if seen is not None:
            seen.update(req)
        signer = key_override or kp
        return _server_reply(
            signer,
            ok=ok,
            key_id=req["key_id"],
            nonce=req.get("nonce", ""),
            error=error,
        )

    return handler


@requires_nats
def test_register_persists_key_on_ack(tmp_path):
    cfg_path = tmp_path / "responder-config.json"
    token = f"approver-1.{uuid.uuid4().hex}"
    server = crypto.generate_keypair()
    seen = {}

    async def body():
        async with connect() as handler_bus:
            await handler_bus.reply("registrations", _fake_handler(server, seen=seen))
            return await responder.register(token, config_path=cfg_path, timeout=2.0)

    reply = run_async(body())
    assert reply["ok"] is True
    assert seen["key_id"] == "approver-1"

    cfg = Config.load(cfg_path)
    assert cfg["key_id"] == "approver-1"
    assert cfg["public_key"] == seen["pubkey"]
    # Persisted private key matches the registered public key.
    assert crypto.KeyPair.from_private_b64(cfg["private_key"]).public_b64() == seen["pubkey"]
    # And the handler's public key is now pinned for the next registration.
    assert cfg["server_key"] == server.public_b64()


@requires_nats
def test_register_does_not_persist_on_rejection(tmp_path):
    cfg_path = tmp_path / "responder-config.json"
    token = f"approver-1.{uuid.uuid4().hex}"
    server = crypto.generate_keypair()

    async def body():
        async with connect() as handler_bus:
            await handler_bus.reply(
                "registrations", _fake_handler(server, ok=False, error="expired")
            )
            return await responder.register(token, config_path=cfg_path, timeout=2.0)

    reply = run_async(body())
    assert reply["ok"] is False
    assert not cfg_path.exists()  # a rejected registration must not clobber config


@requires_nats
def test_register_refuses_an_unsigned_reply(tmp_path):
    """An answer nobody signed is an answer from anybody on the bus."""
    cfg_path = tmp_path / "responder-config.json"
    token = f"approver-1.{uuid.uuid4().hex}"

    async def body():
        async with connect() as handler_bus:
            async def handler(req):
                return {"v": protocol.PROTOCOL_VERSION, "ok": True, "key_id": req["key_id"]}

            await handler_bus.reply("registrations", handler)
            return await responder.register(token, config_path=cfg_path, timeout=2.0)

    with pytest.raises(responder.ServerReplyError):
        run_async(body())
    assert not cfg_path.exists()


@requires_nats
def test_register_refuses_a_handler_that_changed_its_key(tmp_path):
    """The point of pinning: a second handler cannot take over a registered slot."""
    cfg_path = tmp_path / "responder-config.json"
    token = f"approver-1.{uuid.uuid4().hex}"
    known = crypto.generate_keypair()
    Config(
        cfg_path,
        {
            "v": protocol.PROTOCOL_VERSION,
            "key_id": "approver-1",
            "key_type": crypto.ED25519,
            "private_key": crypto.generate_keypair().private_b64(),
            "public_key": "OLD==",
            "server_key": known.public_b64(),
        },
    ).save()

    async def body():
        async with connect() as handler_bus:
            imposter = crypto.generate_keypair()
            await handler_bus.reply("registrations", _fake_handler(imposter))
            return await responder.register(token, config_path=cfg_path, timeout=2.0)

    with pytest.raises(responder.ServerReplyError):
        run_async(body())
    assert Config.load(cfg_path)["public_key"] == "OLD=="  # config untouched


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
