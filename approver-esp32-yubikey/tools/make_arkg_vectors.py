"""make_arkg_vectors.py — the ARKG parity vectors (CLAUDE.md §10.11 tier 2, §10.18).

The firmware derives an ARKG public key **on the device** (`components/arkg`), and
the private half of that key exists nowhere except inside a security key. So there
is exactly one way for the derivation to be wrong and exactly one symptom: the key
signs with a private scalar that does not match the public key this device
registered, every reply is rejected by `hook.verify_reply`, and from the desk that
is indistinguishable from a responder that is not answering. Nothing logs it.

Hence this generator. It is a **second, independent implementation** of
ARKG-P256ADD-ECDH (`draft-bradleylundberg-cfrg-arkg`, the instance
`fido2.cose.ARKG_P256_PLACEHOLDER` names) written from the draft, over
``cryptography`` alone — no ``fido2``, so the guard test runs on a bare
``uv sync``. Where ``fido2`` *is* installed, `tests/test_esp32yk_arkg_vectors.py`
cross-checks the endpoint against it, which is the tie-breaker that matters: the
YubiKey's private half is derived by Yubico's code, not by ours.

What it writes:

    host_test/vectors/arkg_vectors.h     the seed key, three derivations with every
                                        intermediate, the expand_message_xmd cases
                                        underneath them, and two synthetic CTAP2
                                        responses in previewSign's shape
    components/arkg/arkg_selftest_vector.h   the one vector that ships in the firmware,
                                        for `key selftest` on the board

**Committed**, the way `uv.lock` and `dependencies.lock` are: a fresh checkout
builds and host-tests with no Python step. `tests/test_esp32yk_arkg_vectors.py`
regenerates into memory on every ``pytest`` run and fails if the committed file is
not what today's implementation produces.

Why the intermediates are in the file and not just the answer: the pipeline is
seven steps long and six of them are pure (a hash, an expand, two HKDF expands, an
HMAC, a reduction). The host tier can run all six with no elliptic curve at all,
which turns "the derived key is wrong" from one failing assertion into the one
step that broke. The two steps it cannot run — ECDH and the point addition — are
what the device's own ``key selftest`` covers, against this same vector.

Run it (the venv, not the ``py`` launcher — this needs ``cryptography``):

    .venv\\Scripts\\python.exe approver-esp32-yubikey\\tools\\make_arkg_vectors.py

``--check`` writes nothing and exits 1 if the file on disk is stale, which is what
the pytest guard reports in one line.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

_REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO))

HEADER = _REPO / "approver-esp32-yubikey" / "host_test" / "vectors" / "arkg_vectors.h"
#: The one vector that ships inside the firmware — what `key selftest` runs the two
#: curve operations against, on the chip, with nothing plugged in. Committed the way
#: `components/crypto/selftest_vector.h` is, and for the same reason.
SELFTEST_HEADER = _REPO / "approver-esp32-yubikey" / "components" / "arkg" / "arkg_selftest_vector.h"

# --- the curve, spelled out ----------------------------------------------------
#: secp256r1's field prime and the order of its prime-order subgroup. Written here
#: rather than read off ``ec.SECP256R1()`` because the firmware has them as
#: literals too, and two copies of a constant that disagree is a class of bug this
#: whole tier exists to catch.
P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
A = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFC

#: COSE algorithm identifiers, as `fido2.cose` has them. All three are placeholder
#: values in a draft, which is exactly why they are pinned in a generated file.
ALG_ARKG_P256 = -65700  # the seed key's own alg
ALG_ES256 = -7  # the credential's own alg, and the only non-placeholder here
ALG_ESP256 = -9  # the derived key's alg (fully-specified ES256)
ALG_ESP256_SPLIT_ARKG = -65539  # COSE_Sign_Args' alg

#: The two domain separation tags this instance is parameterised with (§10.18).
DST_EXT_BL = b"ARKG-P256"
DST_EXT_KEM = b"ARKG-ECDH.ARKG-P256"

#: `hash_to_field`'s output width for P-256 at a 128-bit security level:
#: ceil((256 + 128) / 8).
HTF_L = 48

#: The purpose label this device scopes its key to. The same string
#: `approver/responder_yubikey.py` uses (`DEFAULT_CTX`) — the same purpose, and a
#: device is told apart from that responder by its `ikm`, not by its `ctx`.
DEVICE_CTX = "ai-remote-approvals"


# --- a deterministic seed key -------------------------------------------------
def _scalar(label: bytes) -> int:
    """A fixed, non-trivial private scalar. Deterministic so the vectors are."""
    return int.from_bytes(hashlib.sha256(label).digest(), "big") % (N - 1) + 1


#: The seed key pair's two private halves. **In the generator only**: they are what
#: lets the Python side check a full sign/verify round trip (a derived private key
#: is `sk_bl + tau mod N`), and they are deliberately not rendered into the header
#: — the device never sees a private scalar, and a header carrying one would invite
#: a test that pretended it could.
SEED_BL_SCALAR = _scalar(b"ai-remote arkg vector: blinding key")
SEED_KEM_SCALAR = _scalar(b"ai-remote arkg vector: kem key")


def _private(scalar: int) -> ec.EllipticCurvePrivateKey:
    return ec.derive_private_key(scalar, ec.SECP256R1())


def _point(key: ec.EllipticCurvePublicKey) -> bytes:
    return key.public_bytes(Encoding.X962, PublicFormat.UncompressedPoint)


def _compressed(key: ec.EllipticCurvePublicKey) -> bytes:
    return key.public_bytes(Encoding.X962, PublicFormat.CompressedPoint)


def _from_point(point: bytes) -> ec.EllipticCurvePublicKey:
    return ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), point)


# --- CBOR, as much of it as a COSE key needs ----------------------------------
def _cbor_head(major: int, value: int) -> bytes:
    if value < 24:
        return bytes([(major << 5) | value])
    if value < 0x100:
        return bytes([(major << 5) | 24, value])
    if value < 0x10000:
        return bytes([(major << 5) | 25]) + struct.pack(">H", value)
    if value < 0x100000000:
        return bytes([(major << 5) | 26]) + struct.pack(">I", value)
    return bytes([(major << 5) | 27]) + struct.pack(">Q", value)


def _cbor_int(value: int) -> bytes:
    return _cbor_head(0, value) if value >= 0 else _cbor_head(1, -value - 1)


def _cbor_bytes(data: bytes) -> bytes:
    return _cbor_head(2, len(data)) + data


def _cbor_map(items: list[tuple[int, bytes]]) -> bytes:
    """A canonical CBOR map with integer keys, in CTAP2's ordering.

    Canonical ordering for CTAP2 is by the *encoded* key, which for the integers
    COSE uses puts the non-negatives first in ascending order and the negatives
    after them in descending numeric order (-1, -2, -3): `1, 3, -1, -2, -3`, which
    is what every COSE key on the wire looks like.
    """
    ordered = sorted(items, key=lambda kv: _cbor_int(kv[0]))
    out = _cbor_head(5, len(ordered))
    for key, value in ordered:
        out += _cbor_int(key) + value
    return out


def _cose_ec2(point: bytes, alg: int) -> bytes:
    """A COSE_Key for a P-256 public key: `kty: EC2, alg, crv: P-256, x, y`."""
    x, y = point[1:33], point[33:65]
    return _cbor_map(
        [
            (1, _cbor_int(2)),
            (3, _cbor_int(alg)),
            (-1, _cbor_int(1)),
            (-2, _cbor_bytes(x)),
            (-3, _cbor_bytes(y)),
        ]
    )


def seed_key_cbor() -> bytes:
    """The ARKG seed public key in the shape ``previewSign`` returns it.

    `kty: 7` is COSE's "generic key with an algorithm-specific structure", which is
    what the ARKG draft registers; `-1` is the blinding key, `-2` the KEM key, and
    `-3` the algorithm the *derived* keys will have.
    """
    return _cbor_map(
        [
            (1, _cbor_int(7)),
            (3, _cbor_int(ALG_ARKG_P256)),
            (-1, _cose_ec2(_point(_private(SEED_BL_SCALAR).public_key()), ALG_ESP256)),
            (-2, _cose_ec2(_point(_private(SEED_KEM_SCALAR).public_key()), ALG_ESP256)),
            (-3, _cbor_int(ALG_ESP256)),
        ]
    )


def args_cbor(key_handle: bytes, ctx: bytes) -> bytes:
    """COSE_Sign_Args — what the authenticator is handed to rebuild the private half.

    `{3: alg, -1: kh, -2: ctx}`, and it goes into the ``previewSign`` extension's
    key 7 (`additionalArgs`) verbatim.
    """
    return _cbor_map(
        [
            (3, _cbor_int(ALG_ESP256_SPLIT_ARKG)),
            (-1, _cbor_bytes(key_handle)),
            (-2, _cbor_bytes(ctx)),
        ]
    )


# --- the primitives, from the draft -------------------------------------------
def expand_message_xmd(msg: bytes, dst: bytes, length: int) -> bytes:
    """RFC 9380 §5.3.1, with SHA-256. `b_in_bytes` 32, `s_in_bytes` 64."""
    if len(dst) > 255 or length > 65535:
        raise ValueError("expand_message_xmd: input out of range")
    ell = -(-length // 32)
    if ell > 255:
        raise ValueError("expand_message_xmd: too much output requested")

    dst_prime = dst + bytes([len(dst)])
    msg_prime = b"\x00" * 64 + msg + struct.pack(">H", length) + b"\x00" + dst_prime
    b_0 = hashlib.sha256(msg_prime).digest()

    # b_1 = H(b_0 || 01 || DST'), and every b_i after it is
    # H(strxor(b_0, b_{i-1}) || i || DST'). Written out rather than folded so the
    # first block's missing xor is visible instead of implied.
    out = b""
    previous = b_0
    for i in range(1, ell + 1):
        block = b_0 if i == 1 else bytes(x ^ y for x, y in zip(b_0, previous))
        previous = hashlib.sha256(block + bytes([i]) + dst_prime).digest()
        out += previous
    return out[:length]


def hash_to_scalar(msg: bytes, dst: bytes) -> int:
    """`hash_to_field(msg, 1)` over GF(N) — one scalar, `HTF_L` bytes reduced."""
    return int.from_bytes(expand_message_xmd(msg, dst, HTF_L), "big") % N


def hkdf_extract(ikm: bytes) -> bytes:
    """HKDF-Extract with an unset salt, which RFC 5869 defines as `HashLen` zeros."""
    return hmac.new(b"\x00" * 32, ikm, hashlib.sha256).digest()


def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    out = b""
    block = b""
    counter = 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        out += block
        counter += 1
    return out[:length]


def point_add(p1: bytes, p2: bytes) -> bytes:
    """Affine addition on secp256r1, uncompressed point in and out.

    The doubling branch is here for completeness rather than for this pipeline:
    `pk_bl + tau*G` hits it only if `tau*G` happens to equal `pk_bl`, which is a
    2^-256 event and still must not silently produce a wrong point.
    """
    x1, y1 = int.from_bytes(p1[1:33], "big"), int.from_bytes(p1[33:65], "big")
    x2, y2 = int.from_bytes(p2[1:33], "big"), int.from_bytes(p2[33:65], "big")
    if x1 == x2 and (y1 + y2) % P == 0:
        raise ValueError("point_add: the sum is the point at infinity")
    if x1 == x2 and y1 == y2:
        lam = (3 * x1 * x1 + A) * pow(2 * y1, -1, P) % P
    else:
        lam = (y2 - y1) * pow(x2 - x1, -1, P) % P
    x3 = (lam * lam - x1 - x2) % P
    y3 = (lam * (x1 - x3) - y1) % P
    return b"\x04" + x3.to_bytes(32, "big") + y3.to_bytes(32, "big")


# --- ARKG-Derive-Public-Key ----------------------------------------------------
def derive(ikm: bytes, ctx: bytes) -> dict:
    """One derivation, every intermediate kept.

    The draft's ARKG-Derive-Public-Key, with the KEM's own steps unrolled so each
    has a name the C side can be asked about:

        ctx'    = I2OSP(LEN(ctx), 1) || ctx
        ctx_bl  = 'ARKG-Derive-Key-BL.'  || ctx'
        ctx_kem = 'ARKG-Derive-Key-KEM.' || ctx'
        (ikm_tau, kh) = KEM-Encaps(pk_kem, ikm, ctx_kem)
        tau = BL-PRF(ikm_tau, ctx_bl)
        pk' = pk_bl + tau * G
    """
    if len(ctx) > 64:
        raise ValueError("ctx is at most 64 bytes")
    if len(ikm) < 32:
        raise ValueError("ikm is at least 32 bytes (the draft's 256-bit floor)")

    ctx_prime = bytes([len(ctx)]) + ctx
    ctx_bl = b"ARKG-Derive-Key-BL." + ctx_prime
    ctx_kem = b"ARKG-Derive-Key-KEM." + ctx_prime

    # KEM-Encaps, over the sub-KEM (ECDH) plus the HMAC wrapper the draft puts
    # around it. `ctx_sub` is threaded through the sub-KEM in the draft and is not
    # consumed by ECDH itself, which is why it does not appear below.
    ephemeral_scalar = hash_to_scalar(ikm, b"ARKG-KEM-ECDH-KG." + DST_EXT_KEM)
    ephemeral = _private(ephemeral_scalar)
    ephemeral_point = _point(ephemeral.public_key())
    ecdh_secret = ephemeral.exchange(ec.ECDH(), _private(SEED_KEM_SCALAR).public_key())

    prk = hkdf_extract(ecdh_secret)
    mac_key = hkdf_expand(prk, b"ARKG-KEM-HMAC-mac." + DST_EXT_KEM + ctx_kem, 32)
    mac_tag = hmac.new(mac_key, ephemeral_point, hashlib.sha256).digest()[:16]
    shared = hkdf_expand(
        prk, b"ARKG-KEM-HMAC-shared." + DST_EXT_KEM + ctx_kem, len(ecdh_secret)
    )
    key_handle = mac_tag + ephemeral_point

    tau = hash_to_scalar(shared, b"ARKG-BL-EC." + DST_EXT_BL + ctx_bl)
    blind_point = _point(_private(tau).public_key())
    derived_point = point_add(_point(_private(SEED_BL_SCALAR).public_key()), blind_point)

    return {
        "ikm": ikm,
        "ctx": ctx,
        "ctx_bl": ctx_bl,
        "ctx_kem": ctx_kem,
        "ephemeral_scalar": ephemeral_scalar.to_bytes(32, "big"),
        "ephemeral_point": ephemeral_point,
        "ecdh_secret": ecdh_secret,
        "mac_key": mac_key,
        "mac_tag": mac_tag,
        "shared": shared,
        "tau": tau.to_bytes(32, "big"),
        "blind_point": blind_point,
        "key_handle": key_handle,
        "derived_point": derived_point,
        "derived_compressed": _compressed(_from_point(derived_point)),
        "derived_public_b64": base64.b64encode(
            _compressed(_from_point(derived_point))
        ).decode("ascii"),
        "args_cbor": args_cbor(key_handle, ctx),
        # The private half, for the Python side's round trip only. Not rendered.
        "derived_private_scalar": (SEED_BL_SCALAR + tau) % N,
    }


# --- two synthetic CTAP2 responses ---------------------------------------------
#
# The firmware's `previewSign` parsers read a shape nobody here has seen on real
# hardware yet (§10.18 — no key has been plugged in). Writing the expected bytes by
# hand in a C++ test would pin *my reading of the draft* twice; building them here
# pins it once, next to the CBOR writer that also has to agree with `fido2` (which
# `tests/test_esp32yk_arkg_vectors.py` checks for the args map).
#
# When a real key does answer, the first thing to compare it against is these.

#: The relying party this device enrols under. A copy of `ctap2.h`'s constant, and
#: the `rpIdHash` below is what makes a wrong copy visible rather than silent.
RP_ID = "approver-esp32-yubikey"

FLAG_UP = 0x01
FLAG_AT = 0x40
FLAG_ED = 0x80

#: The credential this device is enrolled with, and the key handle of the ARKG seed
#: key generated alongside it. Fixed blobs: their content is meaningless, their
#: *positions* in the response are the whole point.
CREDENTIAL_ID = bytes.fromhex("a1b2c3d4") + b"credential-id-of-the-device"
SEED_KEY_HANDLE = bytes.fromhex("0f1e2d3c") + b"key-handle-of-the-generated-key"

#: A DER-shaped blob standing in for a signature. **Not a real one**: an ECDSA
#: signature is randomised, so a real one here would make this generator produce a
#: different file on every run and the staleness guard would fail forever. What the
#: parsers are being tested on is extraction, and the verification that would need a
#: real signature needs a real key — which is the device tier.
SIGN_EXTENSION_SIGNATURE = bytes([0x30, 0x44, 0x02, 0x20]) + bytes(range(32)) + bytes(
    [0x02, 0x20]
) + bytes(range(32, 64))
ASSERTION_SIGNATURE = bytes([0x30, 0x06, 0x02, 0x01, 0x2A, 0x02, 0x01, 0x2B])


def _cbor_text(text: bytes) -> bytes:
    return _cbor_head(3, len(text)) + text


def _cbor_array(items: list[bytes]) -> bytes:
    out = _cbor_head(4, len(items))
    for item in items:
        out += item
    return out


def _cbor_text_map(items: list[tuple[bytes, bytes]]) -> bytes:
    """A map with text keys, canonically ordered: shorter keys first, then bytewise."""
    ordered = sorted(items, key=lambda kv: (len(kv[0]), kv[0]))
    out = _cbor_head(5, len(ordered))
    for key, value in ordered:
        out += _cbor_text(key) + value
    return out


def _auth_data(flags: int, *, attested: bytes = b"", extensions: bytes = b"") -> bytes:
    """`rpIdHash || flags || signCount || [attested] || [extensions]`."""
    return (
        hashlib.sha256(RP_ID.encode()).digest()
        + bytes([flags])
        + struct.pack(">I", 7)
        + attested
        + extensions
    )


def _attested(credential_id: bytes, cose_key: bytes) -> bytes:
    return (
        bytes(16)  # aaguid — all zeros, which a key is allowed to send
        + struct.pack(">H", len(credential_id))
        + credential_id
        + cose_key
    )


def make_credential_response() -> bytes:
    """A `makeCredential` answer to `BuildMakeCredentialArkg`, status byte included.

    The generated key travels an odd road and this is it, in one place: response key
    6 is the *unsigned* extension outputs, its `previewSign` entry's key 7 is a whole
    nested attestation object, and that object's authenticator data carries the seed
    key as a credential — id is the key handle, public key is the ARKG key.
    """
    credential_key = _cose_ec2(_point(_private(_scalar(b"credential key")).public_key()), ALG_ES256)

    outer = _auth_data(
        FLAG_AT | FLAG_ED,
        attested=_attested(CREDENTIAL_ID, credential_key),
        # The signed half of the extension's output: the algorithm, and nothing this
        # firmware reads. Present because a key sends it, and because its presence is
        # what sets the ED flag the parser has to step over.
        extensions=_cbor_text_map([(b"previewSign", _cbor_map([(3, _cbor_int(ALG_ESP256_SPLIT_ARKG))]))]),
    )

    generated = _auth_data(FLAG_AT, attested=_attested(SEED_KEY_HANDLE, seed_key_cbor()))
    nested_attestation = _cbor_map(
        [
            (1, _cbor_text(b"none")),
            (2, _cbor_bytes(generated)),
            (3, _cbor_map([])),
        ]
    )

    body = _cbor_map(
        [
            (1, _cbor_text(b"packed")),
            (2, _cbor_bytes(outer)),
            (3, _cbor_map([])),
            (6, _cbor_text_map([(b"previewSign", _cbor_map([(7, _cbor_bytes(nested_attestation))]))])),
        ]
    )
    return bytes([0x00]) + body


def assertion_response() -> bytes:
    """A `getAssertion` answer to `BuildGetAssertionSign`, status byte included.

    Two signatures, and telling them apart is the point: key 3 is the assertion's
    own, over `authData || clientDataHash`, which is what proves a human touched the
    key; the one inside `authData`'s extensions is the **verdict's**, made with the
    derived key, and it is what goes on the wire in §7's reply.
    """
    auth = _auth_data(
        FLAG_UP | FLAG_ED,
        extensions=_cbor_text_map(
            [(b"previewSign", _cbor_map([(6, _cbor_bytes(SIGN_EXTENSION_SIGNATURE))]))]
        ),
    )
    body = _cbor_map(
        [
            (
                1,
                _cbor_text_map(
                    [(b"id", _cbor_bytes(CREDENTIAL_ID)), (b"type", _cbor_text(b"public-key"))]
                ),
            ),
            (2, _cbor_bytes(auth)),
            (3, _cbor_bytes(ASSERTION_SIGNATURE)),
        ]
    )
    return bytes([0x00]) + body


# --- the cases ----------------------------------------------------------------
def _ikm(label: bytes) -> bytes:
    return hashlib.sha256(label).digest()


def derivation_cases() -> list[dict]:
    """Three derivations, and each one is here for a reason.

    * **default** — the shape the device actually produces (§10.18's `ctx`);
    * **empty-ctx** — the length prefix at zero. `I2OSP(0, 1) || ""` is one byte,
      and an implementation that skipped the prefix for an empty string would agree
      with this vector's *inputs* and disagree with its answer;
    * **max-ctx** — 64 bytes, the draft's ceiling. One more and the derivation must
      refuse rather than truncate, which is a test rather than a vector.
    """
    return [
        {"name": "default", "ctx": DEVICE_CTX.encode(), "ikm": _ikm(b"arkg vector ikm 1")},
        {"name": "empty-ctx", "ctx": b"", "ikm": _ikm(b"arkg vector ikm 2")},
        {"name": "max-ctx", "ctx": b"c" * 64, "ikm": _ikm(b"arkg vector ikm 3")},
    ]


def xmd_cases() -> list[dict]:
    """`expand_message_xmd` on its own, at the three widths that matter.

    48 is what `hash_to_field` asks for and the only width the pipeline uses; 32 is
    the single-block case, and 96 crosses `ell = 3` so a strxor written once and
    reused is caught. The DSTs are the real ones, because a DST' with the wrong
    length byte appended is the mistake this pins.
    """
    return [
        {
            "name": "kem-kg",
            "dst": b"ARKG-KEM-ECDH-KG." + DST_EXT_KEM,
            "msg": _ikm(b"arkg vector ikm 1"),
            "length": 48,
        },
        {
            "name": "one-block",
            "dst": b"ARKG-P256-xmd-test",
            "msg": b"",
            "length": 32,
        },
        {
            "name": "three-blocks",
            "dst": b"ARKG-P256-xmd-test",
            "msg": b"abc",
            "length": 96,
        },
    ]


# --- rendering ----------------------------------------------------------------
def _bytes_rows(data: bytes, indent: str) -> str:
    rows = []
    for start in range(0, len(data), 12):
        chunk = data[start : start + 12]
        rows.append(indent + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(rows)


def _array(name: str, data: bytes, *, size: str | None = None) -> str:
    bound = size if size is not None else str(len(data))
    return (
        f"inline constexpr uint8_t {name}[{bound}] = {{\n"
        f"{_bytes_rows(data, '    ')}\n"
        f"}};\n"
    )


def _field(name: str, data: bytes, indent: str = "        ") -> str:
    return f"{indent}// {name}\n{indent}{{\n{_bytes_rows(data, indent + '    ')}\n{indent}}},\n"


def _c_string(data: bytes) -> str:
    out = '"'
    for byte in data:
        if byte == 0x22:
            out += '\\"'
        elif byte == 0x5C:
            out += "\\\\"
        elif 0x20 <= byte < 0x7F:
            out += chr(byte)
        else:
            out += f'\\x{byte:02x}""'
    return out + '"'


def render_header() -> str:
    seed = seed_key_cbor()
    bl_point = _point(_private(SEED_BL_SCALAR).public_key())
    kem_point = _point(_private(SEED_KEM_SCALAR).public_key())

    derivations = [dict(case, **derive(case["ikm"], case["ctx"])) for case in derivation_cases()]
    args_max = max(len(d["args_cbor"]) for d in derivations)
    ctx_max = max(len(d["ctx"]) for d in derivations)
    xmds = [dict(case, out=expand_message_xmd(case["msg"], case["dst"], case["length"]))
            for case in xmd_cases()]
    xmd_max = max(len(x["out"]) for x in xmds)

    parts: list[str] = []
    parts.append(
        """// GENERATED FILE -- do not edit by hand.
//
// Produced by `approver-esp32-yubikey/tools/make_arkg_vectors.py` from a second,
// independent implementation of ARKG-P256ADD-ECDH written from the draft
// (CLAUDE.md §10.11 tier 2, §10.18). Edit the generator, run it, and commit what
// it writes; `tests/test_esp32yk_arkg_vectors.py` fails if this file and today's
// Python disagree, so an edit made here is one somebody will have to undo.
//
// **What is pinned and what is not.** Six of the derivation's seven steps are
// pure -- an expand, two HKDF expands, an HMAC, a reduction, a concatenation --
// and the host tier runs every one of them against the numbers below with no
// elliptic curve in the build at all. The two that need a curve, the ECDH and the
// point addition, are the device's `key selftest`, against this same vector.
//
// No private scalar appears here, deliberately: the device never holds one, and a
// header carrying one would invite a test that pretended it could.

#pragma once

#include <cstddef>
#include <cstdint>

namespace arkg_vectors {
"""
    )

    parts.append(
        f"""
// --- the instance ---------------------------------------------------------

// COSE algorithm identifiers. All three are placeholder values in a draft, which
// is exactly why they are pinned here rather than remembered.
inline constexpr int64_t kAlgArkgP256 = {ALG_ARKG_P256};
inline constexpr int64_t kAlgEsp256 = {ALG_ESP256};
inline constexpr int64_t kAlgEsp256SplitArkg = {ALG_ESP256_SPLIT_ARKG};

// The two domain separation tags this instance is parameterised with.
inline constexpr char kDstExtBl[] = {_c_string(DST_EXT_BL)};
inline constexpr char kDstExtKem[] = {_c_string(DST_EXT_KEM)};

// `hash_to_field`'s output width for P-256 at a 128-bit security level.
inline constexpr size_t kHashToFieldBytes = {HTF_L};

// The purpose label this device scopes its key to (§10.18).
inline constexpr char kDeviceCtx[] = {_c_string(DEVICE_CTX.encode())};

// --- the seed key ---------------------------------------------------------

// The ARKG seed public key, in the shape `previewSign` returns it: a COSE key
// whose `-1` is the blinding key, `-2` the KEM key and `-3` the algorithm the
// derived keys carry. **This is what the device stores at enrolment.**
{_array('kSeedKeyCbor', seed)}
// The two points inside it, so a parser can be checked without a derivation.
{_array('kSeedBlPoint', bl_point)}
{_array('kSeedKemPoint', kem_point)}
"""
    )

    parts.append(
        f"""
// --- one derivation, every intermediate -----------------------------------

// The names are the draft's. `key_handle` is `kh` -- the 16-byte MAC tag followed
// by the ephemeral point -- and it is what travels back to the authenticator
// inside COSE_Sign_Args.
struct Derivation {{
    const char *name;
    const char *ctx;  // NUL-terminated; no vector here holds an interior NUL
    size_t ctx_length;
    uint8_t ikm[32];
    uint8_t ephemeral_scalar[32];
    uint8_t ephemeral_point[65];
    uint8_t ecdh_secret[32];
    uint8_t mac_key[32];
    uint8_t mac_tag[16];
    uint8_t shared[32];
    uint8_t tau[32];
    // `tau * G`, which is not part of the draft's output and is here for one
    // reason: it is the input to the addition, so the host tier can run the whole
    // pipeline with both curve operations replayed from these numbers.
    uint8_t blind_point[65];
    uint8_t key_handle[81];
    uint8_t derived_point[65];
    uint8_t derived_compressed[33];
    // What §6 registers, and therefore what `hook.verify_reply` will look up.
    const char *derived_public_b64;
    uint8_t args_cbor[{args_max}];
    size_t args_cbor_length;
}};

inline constexpr size_t kCtxMax = {ctx_max};
"""
    )

    rows = []
    for d in derivations:
        row = ["    {"]
        row.append(f'        "{d["name"]}",')
        row.append(f"        {_c_string(d['ctx'])},")
        row.append(f"        {len(d['ctx'])},")
        for field in (
            "ikm",
            "ephemeral_scalar",
            "ephemeral_point",
            "ecdh_secret",
            "mac_key",
            "mac_tag",
            "shared",
            "tau",
            "blind_point",
            "key_handle",
            "derived_point",
            "derived_compressed",
        ):
            row.append(_field(field, d[field]).rstrip("\n"))
        row.append(f'        "{d["derived_public_b64"]}",')
        padded = d["args_cbor"] + b"\x00" * (args_max - len(d["args_cbor"]))
        row.append(_field("args_cbor", padded).rstrip("\n"))
        row.append(f"        {len(d['args_cbor'])},")
        row.append("    },")
        rows.append("\n".join(row))

    parts.append(
        "inline constexpr Derivation kDerivations[] = {\n" + "\n".join(rows) + "\n};\n"
    )

    parts.append(
        f"""
// --- expand_message_xmd on its own ----------------------------------------

// RFC 9380 §5.3.1 with SHA-256, which is the bottom of the pipeline: get the
// DST' length byte wrong and every scalar above it is wrong in a way no other
// test would localise.
struct XmdVector {{
    const char *name;
    const char *dst;
    size_t dst_length;
    const char *msg;
    size_t msg_length;
    size_t out_length;
    uint8_t out[{xmd_max}];
}};

"""
    )

    rows = []
    for x in xmds:
        padded = x["out"] + b"\x00" * (xmd_max - len(x["out"]))
        rows.append(
            "    {\n"
            f'        "{x["name"]}",\n'
            f"        {_c_string(x['dst'])},\n"
            f"        {len(x['dst'])},\n"
            f"        {_c_string(x['msg'])},\n"
            f"        {len(x['msg'])},\n"
            f"        {x['length']},\n"
            f"{_field('out', padded).rstrip(chr(10))}\n"
            "    },"
        )
    parts.append("inline constexpr XmdVector kXmdVectors[] = {\n" + "\n".join(rows) + "\n};\n")

    make_credential = make_credential_response()
    assertion = assertion_response()
    parts.append(
        f"""
// --- two CTAP2 responses, in the shape `previewSign` produces --------------

// **No key has answered one of these yet** (§10.18 — nothing has been plugged into
// the OTG port). Writing the expected bytes by hand in a C++ test would pin one
// reading of the draft twice; building them next to the CBOR writer that also has
// to agree with `fido2` pins it once. When a real key does answer, this is the
// first thing to compare its bytes against.

// The relying party these are built under. A copy of `ctap2.h`'s constant, and the
// `rpIdHash` inside the responses is what makes a wrong copy visible.
inline constexpr char kRelyingPartyId[] = {_c_string(RP_ID.encode())};

// The credential this device is enrolled with, and the key handle of the ARKG seed
// key generated alongside it. Their content is meaningless; their *positions* in
// the response are the whole point.
{_array('kCredentialId', CREDENTIAL_ID)}
{_array('kSeedKeyHandle', SEED_KEY_HANDLE)}
// The signature the extension carries inside the assertion's authenticator data —
// the **verdict's**, not the assertion's. DER-shaped and not a real signature: a
// real one is randomised, so it would make this generator write a different file
// every run. Verifying one needs a real key, which is the device tier.
{_array('kSignExtensionSignature', SIGN_EXTENSION_SIGNATURE)}
// And the assertion's own, which proves presence. Deliberately a different length,
// so a parser that returned one where the other was asked for is visible.
{_array('kAssertionSignature', ASSERTION_SIGNATURE)}

// `makeCredential`, answering `BuildMakeCredentialArkg`. Status byte included.
inline constexpr size_t kMakeCredentialResponseLength = {len(make_credential)};
{_array('kMakeCredentialResponse', make_credential)}
// `getAssertion`, answering `BuildGetAssertionSign`. Status byte included.
inline constexpr size_t kAssertionResponseLength = {len(assertion)};
{_array('kAssertionResponse', assertion)}"""
    )

    parts.append("\n}  // namespace arkg_vectors\n")
    return "".join(parts)


def render_selftest_header() -> str:
    """The one vector that ships **in the firmware** (§10.18, `key selftest`).

    The host tier cannot run the ECDH or the point addition — it has no curve. The
    device can, and this is what it runs them against: the same seed key, `ikm` and
    `ctx` as the first derivation above, and the answer Python computed. It is a
    console command because it is the only part of §10.18 that can be checked today,
    on the board, with nothing plugged into the OTG port.

    Deliberately small. The full set of intermediates belongs to the host tier,
    where a failure can be localised without a serial cable.
    """
    case = derivation_cases()[0]
    result = derive(case["ikm"], case["ctx"])
    return f"""// GENERATED FILE -- do not edit by hand.
//
// Produced by `approver-esp32-yubikey/tools/make_arkg_vectors.py`
// (CLAUDE.md §10.11 tier 2, §10.18). `tests/test_esp32yk_arkg_vectors.py` fails if
// this file and today's Python disagree.
//
// **What this is for:** the two steps of the ARKG derivation that need an elliptic
// curve -- the ECDH inside the KEM and the point addition that blinds the key --
// cannot run in the host tier, because there is no curve in that build. They run
// here, on the chip, through PSA Crypto and one mbedTLS call, and `fido::SelfTest`
// compares the result with the number below.
//
// If it does not match, no security key would ever have worked: every verdict this
// device signed would be rejected by `hook.verify_reply` and the failure would look
// exactly like a device that is not answering.

#pragma once

#include <cstddef>
#include <cstdint>

namespace arkg {{
namespace selftest {{

// The seed key's two points, as `previewSign` would have returned them.
inline constexpr uint8_t kBlindingPoint[65] = {{
{_bytes_rows(_point(_private(SEED_BL_SCALAR).public_key()), '    ')}
}};

inline constexpr uint8_t kKemPoint[65] = {{
{_bytes_rows(_point(_private(SEED_KEM_SCALAR).public_key()), '    ')}
}};

// The entropy and the label.
inline constexpr uint8_t kIkm[32] = {{
{_bytes_rows(case['ikm'], '    ')}
}};

inline constexpr char kCtx[] = {_c_string(case['ctx'])};

// And the answer: the derived public key, compressed, plus the base64 §6 would
// register it as -- so a failure can be read off a console next to what the
// handler's allowlist says.
inline constexpr uint8_t kDerivedCompressed[33] = {{
{_bytes_rows(result['derived_compressed'], '    ')}
}};

inline constexpr char kDerivedPublicKeyB64[] = "{result['derived_public_b64']}";

// The key handle the same derivation produces. Checked too: it is what travels back
// to the authenticator, and a device that derived the right public key with the
// wrong handle would fail on the first real approval and nowhere earlier.
inline constexpr uint8_t kKeyHandle[81] = {{
{_bytes_rows(result['key_handle'], '    ')}
}};

}}  // namespace selftest
}}  // namespace arkg
"""


def generated() -> dict[Path, str]:
    """Every file this generator owns, as {path: text}. The pytest guard's input."""
    return {HEADER: render_header(), SELFTEST_HEADER: render_selftest_header()}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate the ARKG parity vectors.")
    parser.add_argument(
        "--check",
        action="store_true",
        help="write nothing; exit 1 if what is on disk is not what would be written",
    )
    args = parser.parse_args(argv)

    stale = []
    for path, text in generated().items():
        current = path.read_text(encoding="utf-8", newline="") if path.exists() else None
        if current == text:
            continue
        stale.append(path)
        if not args.check:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="")
            print(f"wrote {path}")

    if args.check and stale:
        for path in stale:
            print(f"stale: {path}", file=sys.stderr)
        return 1
    if not stale and not args.check:
        print("up to date")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
