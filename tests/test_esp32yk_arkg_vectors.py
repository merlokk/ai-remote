"""The Python half of the ESP32-S3 ARKG parity tier (CLAUDE.md §10.11 tier 2, §10.18).

`approver-esp32-yubikey/host_test/test_arkg.cpp` asserts that the firmware's
derivation reproduces a set of numbers. This asserts the other half — that those
numbers are still what an independent implementation of the draft produces, and
(where the optional extra is installed) that both agree with **Yubico's own**.

That last agreement is the one with teeth. The derivation's whole job is to
produce a public key whose private half a YubiKey can reconstruct from the key
handle. Nobody verifies that the two match: the device registers the public key,
the key signs with the private one, and the only place the disagreement surfaces is
`hook.verify_reply` rejecting every reply — which from the desk is
indistinguishable from a responder that is simply not answering.

So there are three implementations in this file's blast radius and they are
deliberately unrelated:

* the firmware's, in C++ over PSA and one mbedTLS call;
* the generator's, in Python from the draft text, over ``cryptography`` only —
  which is what makes the committed header regenerable on a bare ``uv sync``;
* ``fido2``'s, which is Yubico's, and is the tie-breaker.

The generator is loaded by path because ``approver-esp32-yubikey`` is not an
importable package name (the dashes), the same reason
`test_esp32_vectors.py` gives for the sibling board's.
"""
from __future__ import annotations

import base64
import importlib.util
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
GENERATOR = REPO / "approver-esp32-yubikey" / "tools" / "make_arkg_vectors.py"

REGENERATE = r".venv\Scripts\python.exe approver-esp32-yubikey\tools\make_arkg_vectors.py"


def _load_generator():
    spec = importlib.util.spec_from_file_location("esp32yk_make_arkg_vectors", GENERATOR)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def generator():
    if not GENERATOR.exists():
        pytest.skip(f"{GENERATOR} is missing")
    return _load_generator()


# --- the committed header ------------------------------------------------------
def test_the_committed_vectors_are_what_python_produces_today(generator):
    for path, expected in generator.generated().items():
        assert path.exists(), f"{path.relative_to(REPO)} is missing; run: {REGENERATE}"
        current = path.read_text(encoding="utf-8", newline="")
        assert current == expected, (
            f"{path.relative_to(REPO)} is not what today's Python produces.\n"
            f"Regenerate and commit it:\n    {REGENERATE}"
        )


def test_the_check_mode_agrees_with_the_comparison(generator):
    assert generator.main(["--check"]) == 0


def test_there_are_vectors_to_compare(generator):
    rendered = generator.generated()
    assert len(rendered) == 2
    for path, text in rendered.items():
        assert len(text) > 800, f"{path.name} is suspiciously short"


# --- the derivation, recomputed ------------------------------------------------
def test_every_derivation_is_reproducible(generator):
    """The vectors are computed, not remembered — and computing them twice in one
    process is also the determinism the whole scheme rests on: same ``ikm`` and
    ``ctx``, same key, or a device that reboots is a device with a key nobody
    registered."""
    cases = generator.derivation_cases()
    assert cases, "no derivations"

    names = [case["name"] for case in cases]
    assert len(names) == len(set(names)), "two derivations share a name"

    for case in cases:
        first = generator.derive(case["ikm"], case["ctx"])
        again = generator.derive(case["ikm"], case["ctx"])
        assert first["derived_point"] == again["derived_point"]
        assert first["key_handle"] == again["key_handle"]

        # Shapes, because a 64-byte "point" would sail through every equality
        # check above and be refused by the device at load.
        assert first["derived_point"][0] == 0x04
        assert len(first["derived_point"]) == 65
        assert len(first["derived_compressed"]) == 33
        assert first["derived_compressed"][0] in (0x02, 0x03)
        assert len(first["key_handle"]) == 16 + 65
        assert len(first["tau"]) == 32


def test_a_different_ikm_or_ctx_gives_a_different_key(generator):
    """Unlinkability, which is the property ``ikm`` exists for — and the reason a
    re-enrolment costs a re-registration (§10.18.1)."""
    ikm = bytes(range(32))
    base = generator.derive(ikm, b"ctx-a")
    other_ctx = generator.derive(ikm, b"ctx-b")
    other_ikm = generator.derive(bytes(range(1, 33)), b"ctx-a")

    assert base["derived_point"] != other_ctx["derived_point"]
    assert base["derived_point"] != other_ikm["derived_point"]
    assert base["key_handle"] != other_ikm["key_handle"]
    # A different ctx does not change the KEM's input entropy, so the ephemeral
    # point is the same and only the tag and the blinding move. Asserted rather
    # than assumed: it is what makes ctx a *label* and not a second ikm.
    assert base["ephemeral_point"] == other_ctx["ephemeral_point"]
    assert base["tau"] != other_ctx["tau"]


def test_the_context_ceiling_is_enforced(generator):
    """64 bytes is the draft's limit. One more must refuse rather than truncate: a
    truncated ctx derives a key the authenticator will not reconstruct."""
    generator.derive(bytes(32), b"c" * 64)
    with pytest.raises(ValueError):
        generator.derive(bytes(32), b"c" * 65)


def test_short_ikm_is_refused(generator):
    """The draft's 256-bit entropy floor, enforced rather than suggested — a
    predictable ``ikm`` gives away the derived key."""
    with pytest.raises(ValueError):
        generator.derive(bytes(31), b"ctx")


def test_expand_message_xmd_matches_rfc9380_shapes(generator):
    """The bottom of the pipeline. Length, determinism, and the one thing that is
    easy to get wrong: DST' is the tag plus its own length byte, so two DSTs where
    one is a prefix of the other must not collide."""
    short = generator.expand_message_xmd(b"msg", b"ARKG-P256", 48)
    longer = generator.expand_message_xmd(b"msg", b"ARKG-P2567", 48)
    assert len(short) == 48
    assert short != longer
    assert generator.expand_message_xmd(b"msg", b"ARKG-P256", 48) == short
    # Crossing a block boundary: 96 bytes is ell = 3, where a strxor written once
    # and reused would show up.
    wide = generator.expand_message_xmd(b"msg", b"ARKG-P256", 96)
    assert len(wide) == 96
    # **And it is not a prefix of the shorter output**, which is the surprise in
    # RFC 9380 worth pinning: the requested length is hashed into b_0, so asking
    # for more bytes changes every byte. An implementation that streamed blocks
    # and truncated would pass every other assertion here.
    assert wide[:32] != generator.expand_message_xmd(b"msg", b"ARKG-P256", 32)


def test_the_derived_key_registers_as_a_p256_key(generator):
    """§6's registration field, in the spelling `lib/crypto.py` verifies with.

    The device publishes this string and nothing else about the key; if it were the
    uncompressed point, or web-safe base64, every reply would be rejected by a
    verifier that never saw the signature.
    """
    from lib import crypto

    case = generator.derivation_cases()[0]
    derived = generator.derive(case["ikm"], case["ctx"])

    assert derived["derived_public_b64"] == base64.b64encode(
        derived["derived_compressed"]
    ).decode("ascii")
    assert len(base64.b64decode(derived["derived_public_b64"])) == 33
    # And it is a key that module will actually load: a bad point fails closed in
    # `verify`, which returns False and would make this vacuous — so sign with the
    # private half and require a True.
    keypair = crypto.KeyPair.from_private_b64(
        base64.b64encode(derived["derived_private_scalar"].to_bytes(32, "big")).decode("ascii"),
        crypto.P256,
    )
    assert keypair.public_b64() == derived["derived_public_b64"]


def test_a_signature_by_the_derived_private_key_verifies_as_a_verdict(generator):
    """**The whole chain, minus the hardware.**

    This is what the device will do once a key is on the port: derive, register the
    public half, and answer §7 with a signature the hook checks. Everything here is
    real except who holds the private scalar — on the device that is the security
    key, and the point of the tier above is that the scalar it reconstructs is this
    one.
    """
    from approver import protocol
    from lib import crypto

    case = generator.derivation_cases()[0]
    derived = generator.derive(case["ikm"], case["ctx"])
    private_b64 = base64.b64encode(
        derived["derived_private_scalar"].to_bytes(32, "big")
    ).decode("ascii")

    message = protocol.signing_bytes(
        v=1,
        session_id="4f9a2c1e-77b3-4d0a-9f21-8c6e5b3a1d02",
        nonce="Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI=",
        tool_name="Bash",
        input_sha256="e8b70996597e349ff8cb37b6cbe3ae52f6c64e27bfb3f41edf819c4817501f14",
        behavior="allow",
        updated_input_sha256="",
        ts=1737345600,
        reason="",
    )
    signature = crypto.sign(private_b64, message, crypto.P256)

    assert crypto.verify(derived["derived_public_b64"], message, signature, crypto.P256)
    # And the negative, because a verifier that returned True for everything would
    # pass the line above.
    assert not crypto.verify(
        derived["derived_public_b64"], message + b"x", signature, crypto.P256
    )


# --- and Yubico's own implementation, when it is installed ---------------------
@pytest.mark.skipif(
    importlib.util.find_spec("fido2") is None,
    reason="optional extra not installed: uv sync --extra yubikey",
)
def test_the_generator_agrees_with_fido2(generator):
    """The tie-breaker (§8.3): the same seed key, the same ``ikm``/``ctx``, and
    Yubico's ARKG implementation must land on the same public key, the same key
    handle and the same COSE_Sign_Args bytes.

    ``args_cbor`` is included on purpose. The firmware builds that map itself, and
    canonical CBOR's ordering rule for negative integer keys is exactly the kind of
    detail that produces a map a key rejects for a reason nothing logs.
    """
    from fido2 import cbor
    from fido2.cose import ESP256_SPLIT_ARKG_PLACEHOLDER, CoseKey

    seed = CoseKey.parse(cbor.decode(generator.seed_key_cbor()))
    assert type(seed).__name__ == "ARKG_P256_PLACEHOLDER"

    for case in generator.derivation_cases():
        mine = generator.derive(case["ikm"], case["ctx"])
        public, args = seed.derive_public_key(case["ikm"], case["ctx"])

        theirs = b"\x04" + bytes(public[-2]) + bytes(public[-3])
        assert theirs == mine["derived_point"], case["name"]
        assert bytes(args[-1]) == mine["key_handle"], case["name"]
        assert bytes(args[-2]) == case["ctx"], case["name"]
        assert args[3] == ESP256_SPLIT_ARKG_PLACEHOLDER
        assert cbor.encode(dict(args)) == mine["args_cbor"], case["name"]
