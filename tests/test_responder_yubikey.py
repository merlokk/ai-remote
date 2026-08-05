"""Tests for approver.responder_yubikey — an approval responder keyed by a YubiKey.

No hardware needed. Two stand-ins do the work (both established in §8.4):

* the ARKG seed key is synthetic (reused from test_yubikey_fido2.py), so
  registration and re-derivation are real ARKG math on a fake device;
* the "device signature" comes from a plain P-256 pair whose private half we
  hold — the authenticator's output is an ECDSA-P256 DER signature over
  ``sha256(signing bytes)``, which is byte-for-byte what this produces, and the
  path hook.py takes to verify it is identical.

The positive round trip against real silicon lives in test_yubikey_integration.py.
"""
import base64
import time

import pytest

pytest.importorskip("fido2", reason="optional extra not installed: uv sync --extra yubikey")

from cryptography.hazmat.primitives import hashes  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402

from approver import hook, protocol, responder, responder_yubikey  # noqa: E402
from lib import config as configlib  # noqa: E402
from lib import crypto, yubikey  # noqa: E402
from tests.conftest import requires_nats, run_async  # noqa: E402

# Reused rather than duplicated: the same synthetic ARKG seed key and throwaway
# attestation CA the lib/yubikey.py tests are built on.
from tests.test_yubikey_fido2 import Chain, _der, make_result  # noqa: E402

CTX = "approvals-test"
IKM = bytes.fromhex("5a" * 32)


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


def _derivation(**kw):
    """A synthetic credential plus the key derived from it, as register() would."""
    result = make_result(**kw)
    return result, yubikey.seed_public_key(result, ctx=CTX.encode(), ikm=IKM)


def _saved_credential(tmp_path, **kw):
    result = make_result(**kw)
    path = tmp_path / "cred.json"
    yubikey.save_result(result, path)
    return result, path


def _device_stand_in():
    """A P-256 pair standing in for a derived ARKG key, plus a signer over it.

    ``sign`` returns base64 of the DER ECDSA signature — exactly the shape
    ``responder_yubikey.device_signer`` produces from the authenticator's bytes.
    """
    priv = ec.generate_private_key(ec.SECP256R1())
    numbers = priv.public_key().public_numbers()
    cose = {
        1: 2,
        3: -9,
        -1: 1,
        -2: numbers.x.to_bytes(32, "big"),
        -3: numbers.y.to_bytes(32, "big"),
    }

    def sign(message: bytes) -> str:
        return base64.b64encode(priv.sign(message, ec.ECDSA(hashes.SHA256()))).decode("ascii")

    return sign, yubikey.p256_public_b64(cose)


def _allowlist(key_id, pubkey_b64, key_type=crypto.P256):
    return {key_id: {"pubkey": pubkey_b64, "key_type": key_type}}


# --- config built at registration ----------------------------------------------
def test_config_from_derivation_shape():
    result, derived = _derivation()
    cfg = responder_yubikey.config_from_derivation("approver-yk", result, derived)

    assert cfg["v"] == protocol.PROTOCOL_VERSION
    assert cfg["key_id"] == "approver-yk"
    assert cfg["key_type"] == crypto.P256  # what the hook must verify with
    assert cfg["public_key"] == yubikey.p256_public_b64(derived)
    assert cfg["ctx"] == CTX
    assert bytes.fromhex(cfg["ikm"]) == IKM
    assert cfg["credential"] == yubikey.result_to_dict(result)


def test_config_from_derivation_holds_no_private_key():
    """The headline property: there is no private key to store — it stays on the key."""
    result, derived = _derivation()
    cfg = responder_yubikey.config_from_derivation("approver-yk", result, derived)
    assert "private_key" not in cfg
    assert not any("private" in k for k in cfg)


def test_config_is_json_round_trippable(tmp_path):
    import json

    result, derived = _derivation()
    cfg = responder_yubikey.config_from_derivation("approver-yk", result, derived)
    path = tmp_path / "responder-yubikey-config.json"
    configlib.Config(path, cfg).save()

    assert json.loads(path.read_text(encoding="utf-8"))["key_type"] == crypto.P256


# --- rebuilding the key at serve time ------------------------------------------
def test_derived_from_config_reproduces_the_registered_key():
    result, derived = _derivation()
    cfg = responder_yubikey.config_from_derivation("approver-yk", result, derived)

    restored_result, restored_derived = responder_yubikey.derived_from_config(cfg)

    assert restored_result.key_handle == result.key_handle
    assert restored_derived.derived_public_key == derived.derived_public_key
    assert yubikey.p256_public_b64(restored_derived) == cfg["public_key"]


def test_derived_from_config_rejects_a_tampered_ikm():
    # A different ikm derives a different key, which the hook would never accept —
    # better to refuse at startup than to sign useless replies.
    result, derived = _derivation()
    cfg = responder_yubikey.config_from_derivation("approver-yk", result, derived)
    cfg["ikm"] = "aa" * 32

    with pytest.raises(configlib.ConfigError):
        responder_yubikey.derived_from_config(cfg)


def test_derived_from_config_rejects_a_tampered_ctx():
    result, derived = _derivation()
    cfg = responder_yubikey.config_from_derivation("approver-yk", result, derived)
    cfg["ctx"] = "some-other-purpose"

    with pytest.raises(configlib.ConfigError):
        responder_yubikey.derived_from_config(cfg)


def test_derived_from_config_rejects_a_foreign_public_key():
    result, derived = _derivation()
    cfg = responder_yubikey.config_from_derivation("approver-yk", result, derived)
    _, other_pub = _device_stand_in()
    cfg["public_key"] = other_pub

    with pytest.raises(configlib.ConfigError):
        responder_yubikey.derived_from_config(cfg)


@pytest.mark.parametrize("missing", ["key_id", "public_key", "ctx", "ikm", "credential"])
def test_derived_from_config_requires_every_field(missing):
    result, derived = _derivation()
    cfg = responder_yubikey.config_from_derivation("approver-yk", result, derived)
    del cfg[missing]

    with pytest.raises(configlib.ConfigError) as e:
        responder_yubikey.derived_from_config(cfg)
    assert missing in str(e.value)


def test_derived_from_config_rejects_non_hex_ikm():
    result, derived = _derivation()
    cfg = responder_yubikey.config_from_derivation("approver-yk", result, derived)
    cfg["ikm"] = "not hex"

    with pytest.raises(configlib.ConfigError):
        responder_yubikey.derived_from_config(cfg)


def test_derived_from_config_rejects_a_broken_credential():
    result, derived = _derivation()
    cfg = responder_yubikey.config_from_derivation("approver-yk", result, derived)
    cfg["credential"] = {"v": yubikey.RESULT_FORMAT_VERSION}

    with pytest.raises(yubikey.YubiKeyError):
        responder_yubikey.derived_from_config(cfg)


# --- the device signer ---------------------------------------------------------
def test_device_signer_returns_base64_of_the_device_signature(monkeypatch):
    result, derived = _derivation()
    monkeypatch.setattr(yubikey, "sign_with_derived_key", lambda *a, **k: b"\x30\x06raw")
    monkeypatch.setattr(yubikey, "verify_signature", lambda *a, **k: True)

    sig = responder_yubikey.device_signer(result, derived)(b"signing bytes")

    assert base64.b64decode(sig, validate=True) == b"\x30\x06raw"


def test_device_signer_refuses_a_signature_that_does_not_verify(monkeypatch):
    # A reply the registered public key cannot check is worse than no reply: the hook
    # would reject it anyway. Catch it here, where the operator sees why.
    result, derived = _derivation()
    monkeypatch.setattr(yubikey, "sign_with_derived_key", lambda *a, **k: b"garbage")

    with pytest.raises(yubikey.YubiKeyError):
        responder_yubikey.device_signer(result, derived)(b"signing bytes")


def test_device_signer_passes_the_message_through_as_data(monkeypatch):
    result, derived = _derivation()
    seen = {}

    def fake_sign(res, der, **kwargs):
        seen.update(kwargs)
        return b"sig"

    monkeypatch.setattr(yubikey, "sign_with_derived_key", fake_sign)
    monkeypatch.setattr(yubikey, "verify_signature", lambda *a, **k: True)

    responder_yubikey.device_signer(result, derived)(b"signing bytes")

    # data=, not digest=: the library hashes it and the device signs that digest.
    assert seen["data"] == b"signing bytes"
    assert seen.get("digest") is None


# --- the console prompt --------------------------------------------------------
def _answers(monkeypatch, *lines):
    """Feed ``input()`` exactly these lines; a further read is a test failure."""
    pending = list(lines)

    def fake_input(prompt=""):
        assert pending, f"prompt asked for more input than expected: {prompt!r}"
        return pending.pop(0)

    monkeypatch.setattr("builtins.input", fake_input)
    return pending


@pytest.mark.parametrize("answer,behavior", [("a", "allow"), ("allow", "allow"),
                                             ("d", "deny"), ("deny", "deny")])
def test_prompt_operator_takes_one_keystroke_and_asks_no_reason(monkeypatch, answer, behavior):
    # The decision already costs a touch; a free-text prompt between the keystroke
    # and the touch is one step too many. reason goes out empty (still signed).
    pending = _answers(monkeypatch, answer)

    assert responder_yubikey.prompt_operator(_request()) == (behavior, "", None)
    assert pending == []  # nothing left unread, and nothing extra was asked for


def test_prompt_operator_skips_on_anything_else(monkeypatch):
    _answers(monkeypatch, "s")

    assert responder_yubikey.prompt_operator(_request()) is None


def test_prompt_operator_shows_the_request(monkeypatch, capsys):
    _answers(monkeypatch, "a")

    responder_yubikey.prompt_operator(_request())

    err = capsys.readouterr().err
    assert "rm -rf build" in err and "Bash" in err


def test_signature_fingerprint_is_the_sha256_of_the_signature_bytes():
    import hashlib

    raw = b"\x30\x06 a der signature"
    sig_b64 = base64.b64encode(raw).decode("ascii")

    fp = responder_yubikey.signature_fingerprint(sig_b64)

    assert hashlib.sha256(raw).hexdigest().startswith(fp)
    assert len(fp) == 16  # short enough to read off a console, long enough to match


def test_print_decision_names_the_behavior_and_the_signature(capsys):
    sign, _ = _device_stand_in()
    req = _request()
    reply = responder.build_signed_reply(
        req, behavior="allow", key_id="approver-yk", sign=sign
    )

    responder_yubikey.print_decision(reply)

    err = capsys.readouterr().err
    assert "allow" in err
    assert responder_yubikey.signature_fingerprint(reply["sig"]) in err


def test_print_decision_survives_a_reply_without_a_usable_signature(capsys):
    # It runs after a decision is already signed and about to be sent: it may not
    # raise, whatever it is handed.
    responder_yubikey.print_decision({"behavior": "deny", "sig": "not base64!!"})

    assert "deny" in capsys.readouterr().err


def test_serve_reports_every_signed_decision():
    import inspect

    assert (
        inspect.signature(responder_yubikey.serve).parameters["on_signed"].default
        is responder_yubikey.print_decision
    )


def test_serve_defaults_to_the_reasonless_prompt():
    # responder.prompt_operator would ask for a reason; this responder must not.
    import inspect

    assert (
        inspect.signature(responder_yubikey.serve).parameters["prompt"].default
        is responder_yubikey.prompt_operator
    )


# --- the payoff: hook.py accepts a YubiKey-signed reply ------------------------
def test_hook_accepts_a_reply_signed_by_a_device_key():
    sign, pubkey = _device_stand_in()
    req = _request()

    reply = responder.build_signed_reply(
        req, behavior="allow", key_id="approver-yk", sign=sign, reason="approved on the key"
    )

    assert hook.verify_reply(req, reply, _allowlist("approver-yk", pubkey)) == (True, "ok")
    assert hook.decision_output(reply)["hookSpecificOutput"]["decision"]["behavior"] == "allow"


def test_hook_accepts_a_device_signed_reply_with_updated_input():
    sign, pubkey = _device_stand_in()
    req = _request()

    reply = responder.build_signed_reply(
        req,
        behavior="allow",
        key_id="approver-yk",
        sign=sign,
        updated_input={"command": "npm ci"},
    )

    trusted, why = hook.verify_reply(req, reply, _allowlist("approver-yk", pubkey))
    assert (trusted, why) == (True, "ok")
    assert hook.decision_output(reply)["hookSpecificOutput"]["decision"]["updatedInput"] == {
        "command": "npm ci"
    }


def test_hook_rejects_a_device_signed_reply_that_was_tampered_with():
    sign, pubkey = _device_stand_in()
    req = _request()
    reply = responder.build_signed_reply(
        req, behavior="deny", key_id="approver-yk", sign=sign, reason="no"
    )

    reply["behavior"] = "allow"  # flip the decision after the key signed it

    trusted, why = hook.verify_reply(req, reply, _allowlist("approver-yk", pubkey))
    assert trusted is False
    assert "signature" in why


def test_hook_rejects_a_device_signed_reply_checked_as_ed25519():
    # The scheme is pinned by the allowlist entry, not the reply: a p256 key
    # registered as ed25519 simply never verifies (no algorithm confusion).
    sign, pubkey = _device_stand_in()
    req = _request()
    reply = responder.build_signed_reply(
        req, behavior="allow", key_id="approver-yk", sign=sign
    )

    trusted, _ = hook.verify_reply(
        req, reply, _allowlist("approver-yk", pubkey, crypto.ED25519)
    )
    assert trusted is False


def test_full_approval_handler_reply_passes_the_hook():
    """prompt -> sign on the key -> reply, exercised through the serve() handler."""
    sign, pubkey = _device_stand_in()
    req = _request()
    handler = responder.make_approval_handler(
        key_id="approver-yk", sign=sign, prompt=lambda request: ("allow", "ok", None)
    )

    reply = run_async(handler(req))

    assert hook.verify_reply(req, reply, _allowlist("approver-yk", pubkey)) == (True, "ok")


# --- registration --------------------------------------------------------------
def test_register_refuses_a_malformed_token(tmp_path):
    cfg_path = tmp_path / "responder-yubikey-config.json"
    with pytest.raises(ValueError):
        run_async(
            responder_yubikey.register("no-dot-here", config_path=cfg_path, credential_path=None)
        )
    assert not cfg_path.exists()


def test_register_rejects_a_credential_that_is_not_a_yubikey(tmp_path):
    """--require-yubikey must stop before anything is published or persisted."""
    ch = Chain()
    att_obj, cdh = ch.attestation()
    _, cred_path = _saved_credential(tmp_path, att_obj=att_obj, client_data_hash=cdh)
    cfg_path = tmp_path / "responder-yubikey-config.json"

    with pytest.raises(responder_yubikey.NotAYubiKey) as e:
        run_async(
            responder_yubikey.register(
                "approver-yk.SEKRIT",
                credential_path=cred_path,
                ctx=CTX,
                ikm=IKM,
                config_path=cfg_path,
                require_yubikey=True,
                timeout=1.0,
            )
        )

    assert e.value.check.is_yubikey is False
    assert e.value.check.reasons
    assert not cfg_path.exists()


def test_register_accepts_a_credential_attested_against_supplied_roots(tmp_path, monkeypatch):
    """The same credential passes when pinned to the CA that actually issued it."""
    ch = Chain()
    att_obj, cdh = ch.attestation()
    _, cred_path = _saved_credential(tmp_path, att_obj=att_obj, client_data_hash=cdh)
    cfg_path = tmp_path / "responder-yubikey-config.json"
    sent = {}

    async def fake_request(subject, payload, timeout=None):
        sent["subject"] = subject
        sent["payload"] = payload
        return {"v": protocol.PROTOCOL_VERSION, "ok": True, "key_id": payload["key_id"]}

    _patch_bus(monkeypatch, fake_request)

    reply = run_async(
        responder_yubikey.register(
            "approver-yk.SEKRIT",
            credential_path=cred_path,
            ctx=CTX,
            ikm=IKM,
            config_path=cfg_path,
            require_yubikey=True,
            roots=[_der(ch.root)],
            intermediates=[],
        )
    )

    assert reply["ok"] is True
    assert sent["subject"] == "registrations"
    assert sent["payload"]["key_type"] == crypto.P256
    assert cfg_path.exists()


def _patch_bus(monkeypatch, fake_request):
    """Replace lib.bus.connect with a stub Bus that answers requests locally."""
    from contextlib import asynccontextmanager

    class _StubBus:
        async def request(self, subject, payload, timeout=None):
            return await fake_request(subject, payload, timeout=timeout)

    @asynccontextmanager
    async def fake_connect(servers=None, **kwargs):
        yield _StubBus()

    monkeypatch.setattr(responder_yubikey.bus, "connect", fake_connect)


def test_register_persists_the_config_on_ack(tmp_path, monkeypatch):
    result, cred_path = _saved_credential(tmp_path)
    cfg_path = tmp_path / "responder-yubikey-config.json"
    sent = {}

    async def fake_request(subject, payload, timeout=None):
        sent.update(payload)
        return {"v": protocol.PROTOCOL_VERSION, "ok": True, "key_id": payload["key_id"]}

    _patch_bus(monkeypatch, fake_request)

    reply = run_async(
        responder_yubikey.register(
            "approver-yk.SEKRIT",
            credential_path=cred_path,
            ctx=CTX,
            ikm=IKM,
            config_path=cfg_path,
        )
    )

    assert reply["ok"] is True
    assert sent["key_id"] == "approver-yk"
    assert sent["key_type"] == crypto.P256

    cfg = configlib.Config.load(cfg_path)
    assert cfg["key_id"] == "approver-yk"
    assert cfg["public_key"] == sent["pubkey"]  # the allowlist gets exactly this
    # And the config alone is enough to rebuild the signing key at serve time.
    _, derived = responder_yubikey.derived_from_config(cfg.data)
    assert yubikey.p256_public_b64(derived) == sent["pubkey"]


def test_register_does_not_persist_on_rejection(tmp_path, monkeypatch):
    _, cred_path = _saved_credential(tmp_path)
    cfg_path = tmp_path / "responder-yubikey-config.json"

    async def fake_request(subject, payload, timeout=None):
        return {"v": protocol.PROTOCOL_VERSION, "ok": False, "error": "expired"}

    _patch_bus(monkeypatch, fake_request)

    reply = run_async(
        responder_yubikey.register(
            "approver-yk.SEKRIT",
            credential_path=cred_path,
            ctx=CTX,
            ikm=IKM,
            config_path=cfg_path,
        )
    )

    assert reply["ok"] is False
    assert not cfg_path.exists()  # a rejected registration must not clobber a config


def test_register_generates_a_fresh_ikm_by_default(tmp_path, monkeypatch):
    _, cred_path = _saved_credential(tmp_path)

    async def fake_request(subject, payload, timeout=None):
        return {"v": protocol.PROTOCOL_VERSION, "ok": True, "key_id": payload["key_id"]}

    _patch_bus(monkeypatch, fake_request)

    keys = []
    for name in ("a.json", "b.json"):
        run_async(
            responder_yubikey.register(
                "approver-yk.SEKRIT",
                credential_path=cred_path,
                ctx=CTX,
                config_path=tmp_path / name,
                )
        )
        keys.append(configlib.Config.load(tmp_path / name)["public_key"])

    # Unlinkability: two registrations of the same YubiKey share no public key.
    assert keys[0] != keys[1]


# --- registration over a real bus ----------------------------------------------
@requires_nats
def test_register_round_trips_over_nats(tmp_path):
    import uuid

    _, cred_path = _saved_credential(tmp_path)
    cfg_path = tmp_path / "responder-yubikey-config.json"
    token = f"approver-yk.{uuid.uuid4().hex}"
    seen = {}

    async def body():
        from lib.bus import connect

        async with connect() as handler_bus:
            async def handler(req):
                seen.update(req)
                return {"v": protocol.PROTOCOL_VERSION, "ok": True, "key_id": req["key_id"]}

            await handler_bus.reply("registrations", handler)
            return await responder_yubikey.register(
                token, credential_path=cred_path, ctx=CTX, ikm=IKM,
                config_path=cfg_path, timeout=2.0,
            )

    reply = run_async(body())

    assert reply["ok"] is True
    assert seen["key_type"] == crypto.P256
    # What the handler would put in the allowlist is what the config kept.
    assert configlib.Config.load(cfg_path)["public_key"] == seen["pubkey"]


@requires_nats
def test_registration_through_the_real_handler_fills_the_allowlist(tmp_path):
    """The interop claim: the real §6 handler stores what the hook needs to verify."""
    from approver import registration_handler
    from lib.bus import connect

    handler_cfg = tmp_path / "handler-config.json"
    token = registration_handler.get_token(
        "approver-yk", config_path=handler_cfg, now=int(time.time())
    )
    _, cred_path = _saved_credential(tmp_path)
    cfg_path = tmp_path / "responder-yubikey-config.json"

    async def body():
        async with connect() as b:
            await b.reply("registrations", registration_handler.make_handler(handler_cfg))
            return await responder_yubikey.register(
                token, credential_path=cred_path, ctx=CTX, ikm=IKM,
                config_path=cfg_path, timeout=2.0,
            )

    assert run_async(body())["ok"] is True

    allowlist = hook.allowlist_from_config(configlib.Config.load(handler_cfg).data)
    entry = allowlist["approver-yk"]
    assert entry["key_type"] == crypto.P256  # the scheme the hook will pin to
    assert entry["pubkey"] == configlib.Config.load(cfg_path)["public_key"]


@requires_nats
def test_hook_trusts_a_device_shaped_key_registered_through_the_real_handler(tmp_path):
    """Whole chain minus the silicon: token -> real handler -> allowlist -> hook.

    The key registered here is the stand-in P-256 pair (§8.4) so its signature can be
    produced offline; everything it travels through is the real code.
    """
    from approver import registration_handler
    from lib.bus import connect

    sign, pubkey = _device_stand_in()
    handler_cfg = tmp_path / "handler-config.json"
    token = registration_handler.get_token(
        "approver-yk", config_path=handler_cfg, now=int(time.time())
    )

    async def body():
        async with connect() as b:
            await b.reply("registrations", registration_handler.make_handler(handler_cfg))
            return await b.request(
                "registrations",
                responder.build_registration_request(
                    token, pubkey, int(time.time()), key_type=crypto.P256
                ),
                timeout=2.0,
            )

    assert run_async(body())["ok"] is True

    req = _request()
    reply = responder.build_signed_reply(
        req, behavior="allow", key_id="approver-yk", sign=sign, reason="on the key"
    )
    allowlist = hook.allowlist_from_config(configlib.Config.load(handler_cfg).data)
    assert hook.verify_reply(req, reply, allowlist) == (True, "ok")


# --- CLI fail-safes ------------------------------------------------------------
def test_main_serve_without_a_config_fails_cleanly(tmp_path, capsys):
    code = responder_yubikey.main(["serve", "--config", str(tmp_path / "nope.json")])
    assert code == responder_yubikey.EXIT_ERROR
    assert "register" in capsys.readouterr().err  # tell the operator what to do


def test_main_register_with_a_bad_token_fails_cleanly(tmp_path, capsys):
    code = responder_yubikey.main(
        ["register", "no-dot-here", "--config", str(tmp_path / "cfg.json")]
    )
    assert code == responder_yubikey.EXIT_ERROR
    assert capsys.readouterr().err
