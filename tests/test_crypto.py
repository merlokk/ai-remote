"""Tests for lib.crypto — Ed25519 key generation, signing, verification."""
import base64

import pytest

from lib.crypto import KeyPair, generate_keypair, sign, verify


def test_generate_produces_32_byte_raw_keys():
    kp = generate_keypair()
    assert len(base64.b64decode(kp.private_b64())) == 32
    assert len(base64.b64decode(kp.public_b64())) == 32


def test_generate_is_not_constant():
    assert generate_keypair().private_b64() != generate_keypair().private_b64()


def test_sign_then_verify_roundtrip():
    kp = generate_keypair()
    msg = b"approve: rm -rf build"
    sig = kp.sign(msg)
    assert verify(kp.public_b64(), msg, sig) is True


def test_verify_rejects_wrong_message():
    kp = generate_keypair()
    sig = kp.sign(b"allow")
    assert verify(kp.public_b64(), b"deny", sig) is False


def test_verify_rejects_wrong_key():
    signer = generate_keypair()
    other = generate_keypair()
    sig = signer.sign(b"payload")
    assert verify(other.public_b64(), b"payload", sig) is False


def test_verify_rejects_tampered_signature():
    kp = generate_keypair()
    sig = kp.sign(b"payload")
    raw = bytearray(base64.b64decode(sig))
    raw[0] ^= 0x01
    tampered = base64.b64encode(bytes(raw)).decode("ascii")
    assert verify(kp.public_b64(), b"payload", tampered) is False


def test_signature_is_deterministic():
    # Ed25519 signatures are deterministic: same key + message => same signature.
    kp = generate_keypair()
    assert kp.sign(b"same") == kp.sign(b"same")


def test_from_private_b64_reconstructs_same_public_key():
    kp = generate_keypair()
    restored = KeyPair.from_private_b64(kp.private_b64())
    assert restored.public_b64() == kp.public_b64()


def test_from_private_b64_signs_compatibly():
    kp = generate_keypair()
    restored = KeyPair.from_private_b64(kp.private_b64())
    msg = b"cross-check"
    assert verify(kp.public_b64(), msg, restored.sign(msg)) is True


def test_module_sign_matches_keypair_sign():
    kp = generate_keypair()
    assert sign(kp.private_b64(), b"x") == kp.sign(b"x")


@pytest.mark.parametrize(
    "pub, sig",
    [
        ("not base64 !!!", "also not base64"),   # malformed base64
        (base64.b64encode(b"short").decode(), base64.b64encode(b"x" * 64).decode()),  # wrong key length
    ],
)
def test_verify_is_fail_safe_on_bad_input(pub, sig):
    # Fail-safe (CLAUDE.md §7): any malformed input must return False, never raise.
    assert verify(pub, b"msg", sig) is False


def test_verify_bad_signature_length_returns_false():
    kp = generate_keypair()
    bad_sig = base64.b64encode(b"tooshort").decode("ascii")
    assert verify(kp.public_b64(), b"msg", bad_sig) is False
