"""Ed25519 key generation, signing and verification (via ``cryptography``).

Keys and signatures cross the wire and the config files as standard base64
(see CLAUDE.md §6/§7): a raw Ed25519 private/public key is 32 bytes, a signature
64 bytes. This module stays protocol-agnostic — it signs and verifies opaque
``bytes``; assembling the "signing bytes" of §7 is the caller's job.

``verify`` is deliberately fail-safe: any malformed input (bad base64, wrong key
or signature length, bad signature) returns ``False`` rather than raising, so the
hook's "any error → interactive prompt / deny" path never trips over an exception.
"""
from __future__ import annotations

import base64
import binascii

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)


def _b64encode(raw: bytes) -> str:
    return base64.b64encode(raw).decode("ascii")


def _b64decode(text: str) -> bytes:
    # validate=True rejects non-base64 junk instead of silently ignoring it.
    return base64.b64decode(text, validate=True)


class KeyPair:
    """An Ed25519 key pair; base64 in, base64 out."""

    def __init__(self, private_key: Ed25519PrivateKey):
        self._private_key = private_key
        self._public_key = private_key.public_key()

    @classmethod
    def generate(cls) -> "KeyPair":
        return cls(Ed25519PrivateKey.generate())

    @classmethod
    def from_private_b64(cls, private_b64: str) -> "KeyPair":
        raw = _b64decode(private_b64)
        return cls(Ed25519PrivateKey.from_private_bytes(raw))

    def private_b64(self) -> str:
        return _b64encode(self._private_key.private_bytes_raw())

    def public_b64(self) -> str:
        return _b64encode(self._public_key.public_bytes_raw())

    def sign(self, message: bytes) -> str:
        """Sign ``message`` and return the base64 signature."""
        return _b64encode(self._private_key.sign(message))


def generate_keypair() -> KeyPair:
    """Generate a fresh Ed25519 key pair."""
    return KeyPair.generate()


def sign(private_b64: str, message: bytes) -> str:
    """Sign ``message`` with a base64-encoded private key; return the base64 signature."""
    return KeyPair.from_private_b64(private_b64).sign(message)


def verify(public_b64: str, message: bytes, sig_b64: str) -> bool:
    """Return True iff ``sig_b64`` is a valid signature of ``message`` for ``public_b64``.

    Fail-safe: returns False for any malformed input, never raises.
    """
    try:
        public_key = Ed25519PublicKey.from_public_bytes(_b64decode(public_b64))
        public_key.verify(_b64decode(sig_b64), message)
        return True
    except (InvalidSignature, ValueError, binascii.Error, TypeError):
        return False
