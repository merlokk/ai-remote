"""Tests for approver.protocol — shared signing-bytes / canonicalization (§7)."""
import hashlib

from approver import protocol


def test_canonical_json_sorts_keys_and_strips_spaces():
    assert protocol.canonical_json({"b": 1, "a": 2}) == '{"a":2,"b":1}'


def test_canonical_json_nested_is_stable():
    obj = {"z": {"y": 1, "x": 2}, "a": [3, 2, 1]}
    assert protocol.canonical_json(obj) == '{"a":[3,2,1],"z":{"x":2,"y":1}}'


def test_canonical_sha256_matches_manual():
    obj = {"command": "rm -rf build"}
    expected = hashlib.sha256(
        '{"command":"rm -rf build"}'.encode("utf-8")
    ).hexdigest()
    assert protocol.canonical_sha256(obj) == expected


def test_signing_bytes_exact_layout():
    sb = protocol.signing_bytes(
        v=1,
        session_id="s",
        nonce="n",
        tool_name="Bash",
        input_sha256="ih",
        behavior="allow",
        updated_input_sha256="",
        ts=42,
        reason="ok",
    )
    assert sb == b"1\ns\nn\nBash\nih\nallow\n\n42\nok"


def test_signing_bytes_reason_is_last_and_preserves_newlines():
    sb = protocol.signing_bytes(
        v=1,
        session_id="s",
        nonce="n",
        tool_name="Bash",
        input_sha256="ih",
        behavior="deny",
        updated_input_sha256="",
        ts=42,
        reason="line1\nline2",
    )
    # reason is the tail, so an embedded newline stays unambiguous.
    assert sb.endswith(b"\nline1\nline2")
    assert sb.decode("utf-8").rsplit("\n", 1)  # sanity: decodes cleanly
