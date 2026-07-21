"""Shared wire-protocol helpers for the approval flow (CLAUDE.md §7).

Both sides build the signing bytes here so they are guaranteed identical:
``responder.py`` signs them, ``hook.py`` recomputes and verifies. The field
order and the ``\\n`` separator are part of the contract and must not change.
"""
from __future__ import annotations

import hashlib
import json
from typing import Any

PROTOCOL_VERSION = 1


def canonical_json(obj: Any) -> str:
    """Canonical JSON for hashing: sorted keys, no whitespace (§7)."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"))


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_sha256(obj: Any) -> str:
    """Hex sha256 of the canonical JSON encoding of ``obj``."""
    return sha256_hex(canonical_json(obj).encode("utf-8"))


def signing_bytes(
    *,
    v: int,
    session_id: str,
    nonce: str,
    tool_name: str,
    input_sha256: str,
    behavior: str,
    updated_input_sha256: str,
    ts: int,
    reason: str,
) -> bytes:
    """Assemble the exact bytes that get signed/verified (§7 "Signing bytes").

    Layout (``\\n``-joined, utf-8):
        v, session_id, nonce, tool_name, input_sha256, behavior,
        updated_input_sha256, ts, reason

    ``reason`` is last on purpose: it is the only free-text field and may contain
    ``\\n``; as the tail it stays unambiguous. Every other field is newline-free.
    """
    parts = [
        str(v),
        session_id,
        nonce,
        tool_name,
        input_sha256,
        behavior,
        updated_input_sha256,
        str(ts),
        reason,
    ]
    return "\n".join(parts).encode("utf-8")
