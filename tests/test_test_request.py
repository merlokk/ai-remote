"""Tests for tools/test_request.py -- the pure halves, no NATS involved."""
from __future__ import annotations

import base64
import json

import pytest

from approver import hook, protocol
from lib import crypto
from tools import test_request


# --- parse_tool_input -----------------------------------------------------------
def test_command_shorthand_becomes_a_bash_input():
    assert test_request.parse_tool_input(command="ls -la", raw=None) == {"command": "ls -la"}


def test_no_command_uses_the_default():
    assert test_request.parse_tool_input(command=None, raw=None) == {
        "command": test_request.DEFAULT_COMMAND
    }


def test_raw_json_wins_over_command():
    parsed = test_request.parse_tool_input(command="ignored", raw='{"file_path": "x.txt"}')
    assert parsed == {"file_path": "x.txt"}


@pytest.mark.parametrize("raw", ["not json", "[1, 2]", '"a string"', "42"])
def test_bad_raw_input_is_rejected(raw):
    # A JSON array or scalar is not a tool_input; say so instead of sending it.
    with pytest.raises(ValueError):
        test_request.parse_tool_input(command=None, raw=raw)


# --- the payload feeds hook.build_request unchanged -----------------------------
def test_payload_builds_a_real_request():
    payload = test_request.build_payload(
        tool_name="Bash",
        tool_input={"command": "echo hi"},
        session_id="s1",
        cwd=r"C:\work",
        permission_mode="default",
    )
    assert payload["hook_event_name"] == hook.HOOK_EVENT

    request = hook.build_request(payload, nonce="n", ts=123)
    assert request["session_id"] == "s1"
    assert request["tool_name"] == "Bash"
    assert request["cwd"] == r"C:\work"
    # The hash must be the hook's, over the same canonical JSON (§7).
    assert request["input_sha256"] == protocol.canonical_sha256({"command": "echo hi"})


def test_session_ids_are_unique_and_recognisable():
    a, b = test_request.default_session_id(), test_request.default_session_id()
    assert a != b
    assert a.startswith("test-request-")


# --- describe_reply / format_report ---------------------------------------------
def _signed_reply(request: dict, behavior: str, keypair, key_id="k1", reason=""):
    sig = keypair.sign(
        protocol.signing_bytes(
            v=request["v"],
            session_id=request["session_id"],
            nonce=request["nonce"],
            tool_name=request["tool_name"],
            input_sha256=request["input_sha256"],
            behavior=behavior,
            updated_input_sha256="",
            ts=request["ts"],
            reason=reason,
        )
    )
    return {
        "v": request["v"],
        "behavior": behavior,
        "reason": reason,
        "session_id": request["session_id"],
        "tool_name": request["tool_name"],
        "input_sha256": request["input_sha256"],
        "nonce": request["nonce"],
        "ts": request["ts"],
        "key_id": key_id,
        "sig": sig,
    }


def test_a_real_signature_reports_as_trusted_with_its_length():
    keypair = crypto.KeyPair.generate("ed25519")
    payload = test_request.build_payload(
        tool_name="Bash", tool_input={"command": "x"}, session_id="s",
        cwd=".", permission_mode="default",
    )
    request = hook.build_request(payload, nonce="nonce", ts=7)
    reply = _signed_reply(request, "allow", keypair)
    allowlist = {"k1": {"pubkey": keypair.public_b64(), "key_type": "ed25519"}}

    trusted, reason = hook.verify_reply(request, reply, allowlist)
    report = test_request.describe_reply(reply, trusted=trusted, reason=reason)

    assert report["trusted"] is True
    assert report["behavior"] == "allow"
    assert report["signature_bytes"] == 64  # ed25519
    assert report["input_sha256"] == request["input_sha256"]

    rendered = test_request.format_report(report)
    assert "TRUSTED" in rendered
    assert request["input_sha256"] in rendered


def test_signature_fingerprint_matches_the_responder_console():
    # The point of printing it: the operator compares this string with what
    # responder_yubikey.print_decision showed them. Same digest, same shape.
    from approver import responder_yubikey

    keypair = crypto.KeyPair.generate("p256")
    payload = test_request.build_payload(
        tool_name="Bash", tool_input={"command": "x"}, session_id="s",
        cwd=".", permission_mode="default",
    )
    request = hook.build_request(payload, nonce="nonce", ts=7)
    reply = _signed_reply(request, "allow", keypair)

    report = test_request.describe_reply(reply, trusted=True, reason="ok")
    assert report["signature_sha256"] == responder_yubikey.signature_fingerprint(reply["sig"])
    assert f"sha256:{report['signature_sha256']}" in test_request.format_report(report)


def test_a_missing_signature_has_no_fingerprint():
    report = test_request.describe_reply(
        {"behavior": "allow", "key_id": "k1", "sig": ""}, trusted=False, reason="x"
    )
    assert report["signature_sha256"] is None


def test_an_empty_signature_is_called_out_as_unregistered():
    report = test_request.describe_reply(
        {"behavior": "allow", "key_id": "k1", "sig": ""},
        trusted=False,
        reason="signature verification failed",
    )
    assert report["signature_bytes"] == 0
    rendered = test_request.format_report(report)
    assert "NONE" in rendered
    assert "REJECTED" in rendered
    assert "fall back" in rendered


def test_a_malformed_signature_does_not_raise():
    report = test_request.describe_reply(
        {"behavior": "deny", "key_id": "k1", "sig": "!!!not base64!!!"},
        trusted=False,
        reason="signature verification failed",
    )
    assert report["signature_bytes"] == -1
    assert "malformed" in test_request.format_report(report)


def test_updated_input_is_shown_when_present():
    report = test_request.describe_reply(
        {"behavior": "allow", "key_id": "k1", "sig": "", "updated_input": {"command": "npm ci"}},
        trusted=True,
        reason="ok",
    )
    assert "npm ci" in test_request.format_report(report)


def test_dry_run_prints_the_request_and_touches_nothing(capsys):
    # --dry-run exists because shell quoting is the usual reason --input does not
    # arrive intact; it must work with no NATS and no responder anywhere.
    code = test_request.main(
        ["--tool", "Write", "--input", '{"file_path": "x.txt"}', "--dry-run",
         "--config", "no-such-config.json"]
    )
    assert code == test_request.EXIT_OK
    sent = json.loads(capsys.readouterr().out)
    assert sent["tool_name"] == "Write"
    assert sent["tool_input"] == {"file_path": "x.txt"}
    assert sent["input_sha256"] == protocol.canonical_sha256({"file_path": "x.txt"})


def test_dry_run_rejects_broken_json_before_anything_else(capsys):
    assert test_request.main(["--input", "{not json", "--dry-run"]) == test_request.EXIT_ERROR
    assert "not valid JSON" in capsys.readouterr().err


def test_report_is_json_serialisable():
    # --json prints this straight out; a non-serialisable field would only show
    # up at the worst moment.
    report = test_request.describe_reply(
        {"behavior": "deny", "key_id": "k1", "sig": base64.b64encode(b"x" * 64).decode()},
        trusted=True,
        reason="ok",
    )
    assert json.loads(json.dumps(report))["signature_bytes"] == 64
