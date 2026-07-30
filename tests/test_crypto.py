"""Tests for lib.crypto — Ed25519 and P-256 key generation, signing, verification."""
import base64

import pytest

from lib.crypto import ED25519, KEY_TYPES, P256, KeyPair, generate_keypair, sign, verify

# Every scheme-agnostic test runs against both supported key types.
KEY_TYPE = pytest.mark.parametrize("key_type", KEY_TYPES)


def test_generate_produces_32_byte_raw_ed25519_keys():
    kp = generate_keypair(ED25519)
    assert len(base64.b64decode(kp.private_b64())) == 32
    assert len(base64.b64decode(kp.public_b64())) == 32


def test_generate_produces_p256_scalar_and_compressed_point():
    kp = generate_keypair(P256)
    assert len(base64.b64decode(kp.private_b64())) == 32       # raw scalar
    assert len(base64.b64decode(kp.public_b64())) == 33        # compressed point


def test_default_key_type_is_ed25519():
    assert generate_keypair().key_type == ED25519


@KEY_TYPE
def test_generate_is_not_constant(key_type):
    assert generate_keypair(key_type).private_b64() != generate_keypair(key_type).private_b64()


@KEY_TYPE
def test_sign_then_verify_roundtrip(key_type):
    kp = generate_keypair(key_type)
    msg = b"approve: rm -rf build"
    sig = kp.sign(msg)
    assert verify(kp.public_b64(), msg, sig, key_type) is True


@KEY_TYPE
def test_verify_rejects_wrong_message(key_type):
    kp = generate_keypair(key_type)
    sig = kp.sign(b"allow")
    assert verify(kp.public_b64(), b"deny", sig, key_type) is False


@KEY_TYPE
def test_verify_rejects_wrong_key(key_type):
    signer = generate_keypair(key_type)
    other = generate_keypair(key_type)
    sig = signer.sign(b"payload")
    assert verify(other.public_b64(), b"payload", sig, key_type) is False


@KEY_TYPE
def test_verify_rejects_tampered_signature(key_type):
    kp = generate_keypair(key_type)
    sig = kp.sign(b"payload")
    raw = bytearray(base64.b64decode(sig))
    raw[0] ^= 0x01
    tampered = base64.b64encode(bytes(raw)).decode("ascii")
    assert verify(kp.public_b64(), b"payload", tampered, key_type) is False


def test_ed25519_signature_is_deterministic():
    # Ed25519 signatures are deterministic: same key + message => same signature.
    kp = generate_keypair(ED25519)
    assert kp.sign(b"same") == kp.sign(b"same")


@KEY_TYPE
def test_from_private_b64_reconstructs_same_public_key(key_type):
    kp = generate_keypair(key_type)
    restored = KeyPair.from_private_b64(kp.private_b64(), key_type)
    assert restored.public_b64() == kp.public_b64()


@KEY_TYPE
def test_from_private_b64_signs_compatibly(key_type):
    kp = generate_keypair(key_type)
    restored = KeyPair.from_private_b64(kp.private_b64(), key_type)
    msg = b"cross-check"
    assert verify(kp.public_b64(), msg, restored.sign(msg), key_type) is True


@KEY_TYPE
def test_module_sign_matches_verify(key_type):
    kp = generate_keypair(key_type)
    assert verify(kp.public_b64(), b"x", sign(kp.private_b64(), b"x", key_type), key_type)


# --- cross-scheme / unknown-type guards ---------------------------------------
def test_verify_rejects_signature_from_the_other_scheme():
    # A P-256 signature must not verify when checked as Ed25519, and vice versa.
    ed = generate_keypair(ED25519)
    p = generate_keypair(P256)
    assert verify(ed.public_b64(), b"m", ed.sign(b"m"), P256) is False
    assert verify(p.public_b64(), b"m", p.sign(b"m"), ED25519) is False


def test_verify_unknown_key_type_is_fail_safe():
    kp = generate_keypair(ED25519)
    assert verify(kp.public_b64(), b"m", kp.sign(b"m"), "rsa") is False


def test_generate_unknown_key_type_raises():
    with pytest.raises(ValueError):
        generate_keypair("rsa")


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
