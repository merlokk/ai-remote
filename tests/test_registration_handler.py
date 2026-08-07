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
from lib import crypto
from lib.bus import NoResponders, connect
from lib.config import Config, ConfigError
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
    # key_type defaults to ed25519 when the request omits it (backward compatible).
    assert data["clients"]["approver-1"] == {
        "pubkey": "PUB==",
        "key_type": "ed25519",
        "registered_ts": 100,
    }
    assert data["pending_tokens"] == []


def test_valid_token_pins_requested_key_type():
    data = _data(pending=[_pending()])
    req = _req()
    req["key_type"] = "p256"
    rh.handle_registration(data, req, now=100)
    assert data["clients"]["approver-1"]["key_type"] == "p256"


def test_unknown_key_type_rejected_as_bad_request():
    data = _data(pending=[_pending()])
    req = _req()
    req["key_type"] = "rsa"
    reply, changed = rh.handle_registration(data, req, now=100)
    assert reply["ok"] is False and reply["error"] == "bad request"
    assert changed is False
    assert len(data["pending_tokens"]) == 1  # token not spent on a bad request


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
        clients={"approver-1": {"pubkey": "OLD", "key_type": "ed25519", "registered_ts": 1}},
    )
    rh.handle_registration(data, _req(pubkey="NEW=="), now=200)
    assert data["clients"]["approver-1"] == {
        "pubkey": "NEW==",
        "key_type": "ed25519",
        "registered_ts": 200,
    }


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


# --- the server key (§6) --------------------------------------------------------
def test_ensure_server_key_generates_an_ed25519_pair():
    data = _data()
    assert rh.ensure_server_key(data) is True
    key = data["server_key"]
    assert key["key_type"] == protocol.SERVER_KEY_TYPE
    # The stored public half must be the one the stored private half produces.
    assert (
        crypto.KeyPair.from_private_b64(
            key["private_key"], protocol.SERVER_KEY_TYPE
        ).public_b64()
        == key["public_key"]
    )


def test_ensure_server_key_keeps_an_existing_key():
    data = _data()
    rh.ensure_server_key(data)
    before = dict(data["server_key"])
    # Rotating silently would break every approver that pinned the old key.
    assert rh.ensure_server_key(data) is False
    assert data["server_key"] == before


def test_ensure_server_key_rejects_an_inconsistent_key():
    data = _data()
    rh.ensure_server_key(data)
    data["server_key"]["public_key"] = crypto.generate_keypair().public_b64()
    with pytest.raises(ConfigError):
        rh.ensure_server_key(data)


@pytest.mark.parametrize(
    "bad",
    [
        "not-a-dict",
        {"key_type": "ed25519", "public_key": "P"},          # no private half
        {"key_type": "ed25519", "private_key": "not-b64!!"},  # unusable private half
        {"key_type": "p256", "private_key": "P", "public_key": "P"},  # wrong scheme
    ],
)
def test_ensure_server_key_rejects_a_broken_record(bad):
    data = _data()
    data["server_key"] = bad
    with pytest.raises(ConfigError):
        rh.ensure_server_key(data)


def test_load_server_key_persists_it_and_is_stable(tmp_path):
    p = tmp_path / "handler-config.json"
    first = rh.load_server_key(p)
    assert Config.load(p)["server_key"]["public_key"] == first
    assert rh.load_server_key(p) == first  # a restart keeps the same identity


def test_get_token_creates_the_server_key(tmp_path):
    p = tmp_path / "handler-config.json"
    rh.get_token("approver-1", config_path=p, ttl=900, now=1000)
    assert Config.load(p)["server_key"]["public_key"]


def test_regenerate_server_key_replaces_an_existing_one():
    data = _data()
    rh.ensure_server_key(data)
    old = data["server_key"]["public_key"]

    new, previous = rh.regenerate_server_key(data)

    assert previous == old
    assert new != old
    assert data["server_key"]["public_key"] == new
    assert (
        crypto.KeyPair.from_private_b64(
            data["server_key"]["private_key"], protocol.SERVER_KEY_TYPE
        ).public_b64()
        == new
    )


def test_regenerate_server_key_creates_one_when_there_is_none():
    data = _data()
    new, previous = rh.regenerate_server_key(data)
    assert previous is None
    assert data["server_key"]["public_key"] == new


def test_regenerate_server_key_repairs_a_broken_record():
    """The escape hatch: ensure_server_key refuses a corrupt key, this replaces it."""
    data = _data()
    data["server_key"] = "not-a-dict"
    new, previous = rh.regenerate_server_key(data)
    assert previous is None
    assert rh.ensure_server_key(data) is False  # the new one validates
    assert data["server_key"]["public_key"] == new


def test_rotate_server_key_persists_and_leaves_the_allowlist_alone(tmp_path):
    p = tmp_path / "handler-config.json"
    token = rh.get_token("approver-1", config_path=p, ttl=900, now=1000)
    old = Config.load(p)["server_key"]["public_key"]

    new, previous = rh.rotate_server_key(p)

    cfg = Config.load(p)
    assert (previous, cfg["server_key"]["public_key"]) == (old, new)
    # Rotating the handler's identity says nothing about the responder keys the
    # hook verifies, nor about tokens already handed out.
    assert cfg["pending_tokens"][0]["token"] == token
    assert cfg["clients"] == {}


def test_rotation_makes_the_handler_sign_with_the_new_key(tmp_path):
    p = tmp_path / "handler-config.json"
    old = rh.load_server_key(p)
    new, _ = rh.rotate_server_key(p)
    nonce = responder.new_nonce()
    req = responder.build_registration_request(
        "approver-1.NOPE", "PUB==", ts=int(time.time()), nonce=nonce
    )

    reply = run_async(rh.make_handler(p)(req))

    assert responder.verify_server_reply(reply, nonce=nonce, pinned_pubkey=new) == new
    # An approver still pinned to the old key refuses it — that is the cost of a
    # rotation, and it must be loud rather than silent.
    with pytest.raises(responder.ServerReplyError):
        responder.verify_server_reply(reply, nonce=nonce, pinned_pubkey=old)


def test_main_new_server_key_prints_the_key_and_rotates(tmp_path, capsys):
    p = tmp_path / "handler-config.json"
    rh.main(["--config", str(p), "--new-server-key"])
    first = capsys.readouterr().out.strip()
    assert Config.load(p)["server_key"]["public_key"] == first

    assert rh.main(["--config", str(p), "--new-server-key"]) == 0
    out = capsys.readouterr()
    assert out.out.strip() != first  # a second run replaces it
    assert first in out.err  # and says which key it replaced


def test_main_new_server_key_names_the_approvers_that_must_re_register(tmp_path, capsys):
    p = tmp_path / "handler-config.json"
    data = _data(clients={"approver-web": {"pubkey": "P", "key_type": "ed25519"}})
    rh.ensure_server_key(data)  # a key they may already have pinned
    Config(p, data).save()

    rh.main(["--config", str(p), "--new-server-key"])

    assert "approver-web" in capsys.readouterr().err


def test_main_first_server_key_warns_nobody(tmp_path, capsys):
    """Nothing was invalidated: those clients registered before there was a key to pin."""
    p = tmp_path / "handler-config.json"
    Config(p, _data(clients={"approver-web": {"pubkey": "P", "key_type": "ed25519"}})).save()

    rh.main(["--config", str(p), "--new-server-key"])

    assert "register again" not in capsys.readouterr().err


def test_sign_reply_verifies_against_the_published_public_key():
    data = _data()
    rh.ensure_server_key(data)
    reply = rh.sign_reply(data, {"v": V, "ok": True, "key_id": "approver-1"}, _req(), now=500)

    assert reply["server_key"] == data["server_key"]["public_key"]
    assert reply["ts"] == 500
    assert crypto.verify(
        reply["server_key"],
        protocol.registration_reply_signing_bytes(
            v=V, ok=True, key_id="approver-1", nonce="", ts=500, error=""
        ),
        reply["sig"],
        protocol.SERVER_KEY_TYPE,
    )


def test_sign_reply_echoes_the_request_nonce():
    data = _data()
    rh.ensure_server_key(data)
    req = _req()
    req["nonce"] = "bm9uY2U="
    reply = rh.sign_reply(data, {"v": V, "ok": True, "key_id": "approver-1"}, req, now=500)
    assert reply["nonce"] == "bm9uY2U="


@pytest.mark.parametrize("request_", ["not-a-dict", {"nonce": 7}, {}])
def test_sign_reply_uses_an_empty_nonce_when_the_request_has_none(request_):
    # Older approvers send no nonce, and a junk message on the open `registrations`
    # subject has no shape at all — neither may break signing.
    data = _data()
    rh.ensure_server_key(data)
    reply = rh.sign_reply(data, {"v": V, "ok": False, "error": "bad request"}, request_, now=1)
    assert reply["nonce"] == ""


def test_error_replies_are_signed_too():
    data = _data()
    rh.ensure_server_key(data)
    reply = rh.sign_reply(data, rh._error("token unknown"), _req(), now=7)
    assert crypto.verify(
        reply["server_key"],
        protocol.registration_reply_signing_bytes(
            v=V, ok=False, key_id="", nonce="", ts=7, error="token unknown"
        ),
        reply["sig"],
        protocol.SERVER_KEY_TYPE,
    )


def test_make_handler_signs_the_reply_the_approver_verifies(tmp_path):
    p = tmp_path / "handler-config.json"
    token = rh.get_token("approver-1", config_path=p, ttl=900, now=int(time.time()))
    nonce = responder.new_nonce()
    req = responder.build_registration_request(
        token, "PUB==", ts=int(time.time()), nonce=nonce
    )

    reply = run_async(rh.make_handler(p)(req))

    assert reply["ok"] is True
    # The approver's own verification is the contract, so use it rather than a
    # re-implementation of the check.
    assert responder.verify_server_reply(reply, nonce=nonce) == (
        Config.load(p)["server_key"]["public_key"]
    )


def test_make_handler_signs_rejections(tmp_path):
    p = tmp_path / "handler-config.json"
    nonce = responder.new_nonce()
    req = responder.build_registration_request(
        "approver-1.NOPE", "PUB==", ts=int(time.time()), nonce=nonce
    )

    reply = run_async(rh.make_handler(p)(req))

    assert reply["ok"] is False and reply["error"] == "token unknown"
    responder.verify_server_reply(reply, nonce=nonce)  # raises if unsigned/tampered


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
