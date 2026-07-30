"""Key generation, signing and verification (via ``cryptography``).

Two signature schemes are supported, selected by a ``key_type`` tag:

* ``"ed25519"`` (the default) — Ed25519; raw private/public keys are 32 bytes,
  the signature 64 bytes.
* ``"p256"`` — ECDSA over NIST P-256 (secp256r1) with SHA-256; the public key is
  the 33-byte compressed point, the private key the 32-byte scalar, the signature
  DER-encoded (variable length). ECDSA is randomized, so signatures are not
  deterministic (unlike Ed25519).

Keys and signatures cross the wire and the config files as standard base64 (see
CLAUDE.md §6/§7). This module stays protocol-agnostic — it signs and verifies
opaque ``bytes``; assembling the "signing bytes" of §7 is the caller's job. The
``key_type`` is not part of the signed message: it is pinned by the trusted
allowlist entry (bound to ``key_id``), so the verifier always uses the algorithm
that was registered, never one chosen by the reply.

``verify`` is deliberately fail-safe: any malformed input (bad base64, wrong key
or signature length, unknown ``key_type``, bad signature) returns ``False`` rather
than raising, so the hook's "any error → interactive prompt / deny" path never
trips over an exception.
"""
from __future__ import annotations

import base64
import binascii

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ED25519 = "ed25519"
P256 = "p256"
KEY_TYPES = (ED25519, P256)
DEFAULT_KEY_TYPE = ED25519


def _b64encode(raw: bytes) -> str:
    return base64.b64encode(raw).decode("ascii")


def _b64decode(text: str) -> bytes:
    # validate=True rejects non-base64 junk instead of silently ignoring it.
    return base64.b64decode(text, validate=True)


# --- per-scheme backends -------------------------------------------------------
# Each backend maps a key_type onto the raw <-> object conversions and the
# sign/verify primitives. ``KeyPair`` and the module functions dispatch through
# ``_backend(key_type)`` so the rest of the codebase only speaks base64 + key_type.
class _Ed25519Backend:
    key_type = ED25519

    @staticmethod
    def generate():
        return Ed25519PrivateKey.generate()

    @staticmethod
    def load_private(raw: bytes):
        return Ed25519PrivateKey.from_private_bytes(raw)

    @staticmethod
    def private_raw(private_key) -> bytes:
        return private_key.private_bytes_raw()

    @staticmethod
    def public_raw(public_key) -> bytes:
        return public_key.public_bytes_raw()

    @staticmethod
    def load_public(raw: bytes):
        return Ed25519PublicKey.from_public_bytes(raw)

    @staticmethod
    def sign(private_key, message: bytes) -> bytes:
        return private_key.sign(message)

    @staticmethod
    def do_verify(public_key, message: bytes, signature: bytes) -> None:
        public_key.verify(signature, message)


class _P256Backend:
    key_type = P256
    _CURVE = ec.SECP256R1()
    _ALGORITHM = ec.ECDSA(hashes.SHA256())

    @classmethod
    def generate(cls):
        return ec.generate_private_key(cls._CURVE)

    @classmethod
    def load_private(cls, raw: bytes):
        # A raw P-256 scalar is exactly 32 bytes; reject anything else up front so
        # a short/long blob can't be zero-extended into a different key.
        if len(raw) != 32:
            raise ValueError("P-256 private key must be 32 bytes")
        return ec.derive_private_key(int.from_bytes(raw, "big"), cls._CURVE)

    @staticmethod
    def private_raw(private_key) -> bytes:
        return private_key.private_numbers().private_value.to_bytes(32, "big")

    @staticmethod
    def public_raw(public_key) -> bytes:
        return public_key.public_bytes(Encoding.X962, PublicFormat.CompressedPoint)

    @classmethod
    def load_public(cls, raw: bytes):
        return ec.EllipticCurvePublicKey.from_encoded_point(cls._CURVE, raw)

    @classmethod
    def sign(cls, private_key, message: bytes) -> bytes:
        return private_key.sign(message, cls._ALGORITHM)

    @classmethod
    def do_verify(cls, public_key, message: bytes, signature: bytes) -> None:
        public_key.verify(signature, message, cls._ALGORITHM)


_BACKENDS = {ED25519: _Ed25519Backend, P256: _P256Backend}


def _backend(key_type: str):
    try:
        return _BACKENDS[key_type]
    except KeyError:
        raise ValueError(f"unknown key_type {key_type!r}; expected one of {KEY_TYPES}")


class KeyPair:
    """A signing key pair of a given ``key_type``; base64 in, base64 out."""

    def __init__(self, private_key, key_type: str = DEFAULT_KEY_TYPE):
        self.key_type = key_type
        self._backend = _backend(key_type)
        self._private_key = private_key
        self._public_key = private_key.public_key()

    @classmethod
    def generate(cls, key_type: str = DEFAULT_KEY_TYPE) -> "KeyPair":
        return cls(_backend(key_type).generate(), key_type)

    @classmethod
    def from_private_b64(cls, private_b64: str, key_type: str = DEFAULT_KEY_TYPE) -> "KeyPair":
        raw = _b64decode(private_b64)
        return cls(_backend(key_type).load_private(raw), key_type)

    def private_b64(self) -> str:
        return _b64encode(self._backend.private_raw(self._private_key))

    def public_b64(self) -> str:
        return _b64encode(self._backend.public_raw(self._public_key))

    def sign(self, message: bytes) -> str:
        """Sign ``message`` and return the base64 signature."""
        return _b64encode(self._backend.sign(self._private_key, message))


def generate_keypair(key_type: str = DEFAULT_KEY_TYPE) -> KeyPair:
    """Generate a fresh key pair of ``key_type`` (default Ed25519)."""
    return KeyPair.generate(key_type)


def sign(private_b64: str, message: bytes, key_type: str = DEFAULT_KEY_TYPE) -> str:
    """Sign ``message`` with a base64-encoded private key; return the base64 signature."""
    return KeyPair.from_private_b64(private_b64, key_type).sign(message)


def verify(
    public_b64: str, message: bytes, sig_b64: str, key_type: str = DEFAULT_KEY_TYPE
) -> bool:
    """Return True iff ``sig_b64`` is a valid ``key_type`` signature of ``message``.

    Fail-safe: returns False for any malformed input (bad base64, wrong key/sig
    length, unknown ``key_type``, bad signature), never raises.
    """
    try:
        backend = _backend(key_type)
        public_key = backend.load_public(_b64decode(public_b64))
        backend.do_verify(public_key, message, _b64decode(sig_b64))
        return True
    except (InvalidSignature, ValueError, binascii.Error, TypeError):
        return False
