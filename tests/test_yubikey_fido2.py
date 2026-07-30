"""Tests for lib/yubikey.py that need the optional `fido2` extra (CLAUDE.md §8).

Still no physical YubiKey: the ARKG seed key is built synthetically from two
P-256 key pairs (exactly the shape the authenticator returns), and attestation is
exercised against a throwaway CA generated here. Skipped entirely when the extra
is not installed, so a bare `uv sync` still yields a green suite.
"""
import datetime

import pytest

fido2 = pytest.importorskip("fido2", reason="optional extra not installed: uv sync --extra yubikey")

from cryptography import x509  # noqa: E402
from cryptography.hazmat.primitives import hashes, serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402
from cryptography.x509.oid import NameOID  # noqa: E402
from fido2 import cbor  # noqa: E402
from fido2.cose import ES256, ESP256, ESP256_SPLIT_ARKG_PLACEHOLDER, ARKG_P256_PLACEHOLDER, CoseKey  # noqa: E402
from fido2.webauthn import AttestedCredentialData, AuthenticatorData  # noqa: E402
from fido2.webauthn import AttestationObject  # noqa: E402

from lib import yubikey  # noqa: E402

AAGUID = bytes.fromhex("d7781e5de35346aaafe23ca49f13332a")  # YubiKey 5 Series with NFC
OID_AAGUID = x509.ObjectIdentifier("1.3.6.1.4.1.45724.1.1.4")


# --- synthetic ARKG seed key ---------------------------------------------------
def _cose_ec2(pub):
    n = pub.public_numbers()
    return {1: 2, 3: ES256.ALGORITHM, -1: 1, -2: n.x.to_bytes(32, "big"), -3: n.y.to_bytes(32, "big")}


def make_seed_key_cbor():
    """CBOR COSE key in the shape previewSign returns for an ARKG seed key."""
    bl = ec.generate_private_key(ec.SECP256R1())
    kem = ec.generate_private_key(ec.SECP256R1())
    return cbor.encode(
        {
            1: 7,
            3: ARKG_P256_PLACEHOLDER.ALGORITHM,
            -1: _cose_ec2(bl.public_key()),
            -2: _cose_ec2(kem.public_key()),
            -3: ESP256.ALGORITHM,
        }
    )


def make_result(seed_cbor=None, *, key_handle=b"kh-1234", att_obj=None, client_data_hash=b"\x11" * 32):
    return yubikey.MakeCredentialResult(
        key_handle=key_handle,
        seed_public_key_cbor=seed_cbor if seed_cbor is not None else make_seed_key_cbor(),
        algorithm=ESP256_SPLIT_ARKG_PLACEHOLDER,
        credential_id=b"cred-id",
        aaguid=AAGUID,
        client_data_hash=client_data_hash,
        attestation_object=att_obj,
        registration_response=None,
    )


# --- seed_public_key -> derived public key + arkg args -------------------------
def test_seed_public_key_returns_derived_key_and_args():
    result = make_result()
    derived = yubikey.seed_public_key(result, ctx=b"my-ctx")

    assert isinstance(derived.derived_public_key, ESP256)
    assert derived.derived_public_key[3] == ESP256.ALGORITHM
    assert derived.arkg_args[3] == ESP256_SPLIT_ARKG_PLACEHOLDER
    assert derived.arkg_args[-2] == b"my-ctx"
    assert isinstance(derived.arkg_args[-1], bytes) and derived.arkg_args[-1]
    assert derived.ctx == b"my-ctx"
    assert derived.key_handle == b"kh-1234"


def test_seed_public_key_derived_key_is_a_valid_p256_point():
    derived = yubikey.seed_public_key(make_result(), ctx=b"ctx")
    pk = derived.derived_public_key
    ec.EllipticCurvePublicNumbers(
        int.from_bytes(pk[-2], "big"), int.from_bytes(pk[-3], "big"), ec.SECP256R1()
    ).public_key()  # raises if off-curve


def test_seed_public_key_generates_32_byte_ikm_by_default():
    derived = yubikey.seed_public_key(make_result(), ctx=b"ctx")
    assert len(derived.ikm) == 32


def test_seed_public_key_random_ikm_differs_between_calls():
    result = make_result()
    a = yubikey.seed_public_key(result, ctx=b"ctx")
    b = yubikey.seed_public_key(result, ctx=b"ctx")
    assert a.ikm != b.ikm
    assert a.derived_public_key[-2] != b.derived_public_key[-2]


def test_seed_public_key_is_deterministic_for_same_ikm_and_ctx():
    result = make_result()
    ikm = b"\x07" * 32
    a = yubikey.seed_public_key(result, ctx=b"ctx", ikm=ikm)
    b = yubikey.seed_public_key(result, ctx=b"ctx", ikm=ikm)
    assert a.derived_public_key[-2] == b.derived_public_key[-2]
    assert a.arkg_args[-1] == b.arkg_args[-1]


def test_seed_public_key_different_ctx_yields_different_key():
    result = make_result()
    ikm = b"\x07" * 32
    a = yubikey.seed_public_key(result, ctx=b"ctx-a", ikm=ikm)
    b = yubikey.seed_public_key(result, ctx=b"ctx-b", ikm=ikm)
    assert a.derived_public_key[-2] != b.derived_public_key[-2]


def test_seed_public_key_different_ikm_yields_different_key():
    result = make_result()
    a = yubikey.seed_public_key(result, ctx=b"ctx", ikm=b"\x01" * 32)
    b = yubikey.seed_public_key(result, ctx=b"ctx", ikm=b"\x02" * 32)
    assert a.derived_public_key[-2] != b.derived_public_key[-2]


def test_seed_public_key_args_cbor_round_trips():
    derived = yubikey.seed_public_key(make_result(), ctx=b"ctx")
    assert cbor.decode(derived.arkg_args_cbor) == dict(derived.arkg_args)


def test_seed_public_key_exposes_derived_key_cbor():
    derived = yubikey.seed_public_key(make_result(), ctx=b"ctx")
    assert CoseKey.parse(cbor.decode(derived.derived_public_key_cbor)) == derived.derived_public_key


def test_seed_public_key_exposes_the_parsed_seed_key():
    derived = yubikey.seed_public_key(make_result(), ctx=b"ctx")
    assert isinstance(derived.seed_public_key, ARKG_P256_PLACEHOLDER)
    assert isinstance(derived.seed_public_key.pk_bl, CoseKey)
    assert isinstance(derived.seed_public_key.pk_kem, CoseKey)


def test_seed_public_key_rejects_short_ikm():
    with pytest.raises(ValueError):
        yubikey.seed_public_key(make_result(), ctx=b"ctx", ikm=b"\x01" * 31)


def test_seed_public_key_rejects_empty_ctx():
    with pytest.raises(ValueError):
        yubikey.seed_public_key(make_result(), ctx=b"")


def test_seed_public_key_rejects_non_arkg_seed_key():
    # A plain ES256 credential public key cannot seed ARKG derivation.
    priv = ec.generate_private_key(ec.SECP256R1())
    plain = cbor.encode(_cose_ec2(priv.public_key()))
    with pytest.raises(yubikey.ExtensionUnsupported):
        yubikey.seed_public_key(make_result(seed_cbor=plain), ctx=b"ctx")


def test_seed_public_key_rejects_garbage_seed_blob():
    with pytest.raises(yubikey.YubiKeyError):
        yubikey.seed_public_key(make_result(seed_cbor=b"\xff\xff\xff"), ctx=b"ctx")


# --- bundled Yubico trust anchors ---------------------------------------------
def test_load_yubico_roots_returns_the_three_fido_roots():
    roots = yubikey.load_yubico_roots()
    assert len(roots) == 3
    subjects = {x509.load_der_x509_certificate(d).subject.rfc4514_string() for d in roots}
    assert subjects == {
        "CN=Yubico U2F Root CA Serial 457200631",
        "CN=Yubico FIDO Root CA Serial 450203556",
        "CN=Yubico Attestation Root 1",
    }


def test_bundled_root_fingerprints_are_pinned():
    # Guards against a silent swap of lib/yubico-fido-ca.pem.
    expected = {
        "0fa1386f80eb8713263ae5c1d84deb455bdf08aea50ab05503cefee82b092d42",
        "35f1a54b353bfb711e6d42adbeb76c0e9dead095018e6a94783ba2192fd6faad",
        "62760c6a6ef91679f454c8902b80fd009825b3f25da90f1fbace2ec6586cd5a8",
    }
    got = {
        x509.load_der_x509_certificate(d).fingerprint(hashes.SHA256()).hex()
        for d in yubikey.load_yubico_roots()
    }
    assert got == expected


def test_bundled_roots_are_self_signed():
    for der in yubikey.load_yubico_roots():
        c = x509.load_der_x509_certificate(der)
        assert c.subject == c.issuer


# --- synthetic CA + packed attestation ----------------------------------------
def _name(cn, org="Yubico AB", country="SE", ou=None):
    parts = [
        x509.NameAttribute(NameOID.COUNTRY_NAME, country),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, org),
        x509.NameAttribute(NameOID.COMMON_NAME, cn),
    ]
    if ou:
        parts.insert(2, x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, ou))
    return x509.Name(parts)


def _mkcert(subject, issuer_name, issuer_key, signing_key, *, ca, aaguid=None):
    """Build a v3 cert for ``signing_key``'s public half, signed by ``issuer_key``."""
    now = datetime.datetime(2025, 1, 1, tzinfo=datetime.timezone.utc)
    builder = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer_name)
        .public_key(signing_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True)
    )
    if aaguid is not None:
        # OCTET STRING wrapper, as WebAuthn's packed attestation extension expects.
        builder = builder.add_extension(
            x509.UnrecognizedExtension(OID_AAGUID, b"\x04\x10" + aaguid), critical=False
        )
    return builder.sign(issuer_key, hashes.SHA256())


def _der(cert):
    return cert.public_bytes(serialization.Encoding.DER)


class Chain:
    """A throwaway root -> attestation-EE chain plus a signed packed attestation."""

    def __init__(self, *, org="Yubico AB", root_cn="Test Yubico Root", aaguid=AAGUID):
        self.aaguid = aaguid
        root_name = _name(root_cn, org=org)
        self.root_key = ec.generate_private_key(ec.SECP256R1())
        self.root = _mkcert(root_name, root_name, self.root_key, self.root_key, ca=True)

        self.ee_key = ec.generate_private_key(ec.SECP256R1())
        self.ee = _mkcert(
            _name("Test Authenticator EE", org=org, ou="Authenticator Attestation"),
            self.root.subject,
            self.root_key,
            self.ee_key,
            ca=False,
            aaguid=aaguid,
        )

    def attestation(self, *, client_data_hash=b"\x11" * 32, credential_id=b"cred-id"):
        """A `packed` AttestationObject signed by the EE key, plus its clientDataHash."""
        cred_key = ec.generate_private_key(ec.SECP256R1())
        cred = AttestedCredentialData.create(
            self.aaguid, credential_id, ES256.from_cryptography_key(cred_key.public_key())
        )
        auth_data = AuthenticatorData.create(
            b"\x22" * 32,  # rpIdHash — opaque to attestation verification
            AuthenticatorData.FLAG.AT | AuthenticatorData.FLAG.UP,
            0,
            cred,
        )
        sig = self.ee_key.sign(bytes(auth_data) + client_data_hash, ec.ECDSA(hashes.SHA256()))
        att_stmt = {"alg": ES256.ALGORITHM, "sig": sig, "x5c": [_der(self.ee)]}
        return AttestationObject.create("packed", auth_data, att_stmt), client_data_hash


# --- chain verification -------------------------------------------------------
def test_verify_certificate_chain_accepts_chain_to_a_trusted_root():
    ch = Chain()
    root = yubikey.verify_certificate_chain([_der(ch.ee)], [_der(ch.root)])
    assert root.subject == ch.root.subject


def test_verify_certificate_chain_rejects_unrelated_root():
    ch = Chain()
    other = Chain(root_cn="Someone Else Root")
    with pytest.raises(yubikey.AttestationError):
        yubikey.verify_certificate_chain([_der(ch.ee)], [_der(other.root)])


def test_verify_certificate_chain_rejects_empty_chain():
    ch = Chain()
    with pytest.raises(yubikey.AttestationError):
        yubikey.verify_certificate_chain([], [_der(ch.root)])


def test_verify_certificate_chain_rejects_empty_root_set():
    ch = Chain()
    with pytest.raises(yubikey.AttestationError):
        yubikey.verify_certificate_chain([_der(ch.ee)], [])


# --- Yubico identity in the certificate ---------------------------------------
def test_identify_yubico_certificate_accepts_yubico_org():
    ch = Chain(org="Yubico AB")
    ok, detail = yubikey.identify_yubico_certificate(_der(ch.ee))
    assert ok is True
    assert "Yubico" in detail


def test_identify_yubico_certificate_rejects_other_vendor():
    # Neither the subject nor the issuer may name Yubico.
    ch = Chain(org="Acme Security Inc", root_cn="Acme Root")
    ok, detail = yubikey.identify_yubico_certificate(_der(ch.ee))
    assert ok is False
    assert detail


def test_identify_yubico_certificate_accepts_yubico_issuer_only():
    # A real YubiKey EE cert is "Yubico ..." issued by a Yubico root; catching it via
    # the issuer alone must also work.
    ch = Chain(org="Acme Security Inc", root_cn="Yubico Test Root")
    ok, detail = yubikey.identify_yubico_certificate(_der(ch.ee))
    assert ok is True
    assert "issuer" in detail


# --- full attestation check ---------------------------------------------------
def test_verify_attestation_object_accepts_trusted_yubico_like_chain():
    ch = Chain()
    att_obj, cdh = ch.attestation()
    check = yubikey.verify_attestation_object(att_obj, cdh, roots=[_der(ch.root)])
    assert check.is_yubikey is True
    assert check.attestation_verified is True
    assert check.reasons == ()
    assert check.aaguid == AAGUID
    assert check.aaguid_hex == AAGUID.hex()
    assert check.fmt == "packed"
    assert "Yubico" in check.certificate_subject
    assert check.trusted_root_subject == ch.root.subject.rfc4514_string()
    assert check.chain_length == 1


def test_verify_attestation_object_rejects_untrusted_root():
    ch = Chain()
    other = Chain(root_cn="Someone Else Root")
    att_obj, cdh = ch.attestation()
    check = yubikey.verify_attestation_object(att_obj, cdh, roots=[_der(other.root)])
    assert check.is_yubikey is False
    assert any("root" in r.lower() or "chain" in r.lower() for r in check.reasons)


def test_verify_attestation_object_rejects_non_yubico_vendor():
    ch = Chain(org="Acme Security Inc", root_cn="Other Root")
    att_obj, cdh = ch.attestation()
    check = yubikey.verify_attestation_object(att_obj, cdh, roots=[_der(ch.root)])
    assert check.is_yubikey is False
    assert any("yubico" in r.lower() for r in check.reasons)


def test_verify_attestation_object_rejects_wrong_client_data_hash():
    ch = Chain()
    att_obj, _ = ch.attestation(client_data_hash=b"\x11" * 32)
    check = yubikey.verify_attestation_object(att_obj, b"\x99" * 32, roots=[_der(ch.root)])
    assert check.is_yubikey is False
    assert check.attestation_verified is False


def test_verify_attestation_object_honours_aaguid_allowlist():
    ch = Chain()
    att_obj, cdh = ch.attestation()
    ok = yubikey.verify_attestation_object(
        att_obj, cdh, roots=[_der(ch.root)], allowed_aaguids=[AAGUID]
    )
    assert ok.is_yubikey is True

    bad = yubikey.verify_attestation_object(
        att_obj, cdh, roots=[_der(ch.root)], allowed_aaguids=[b"\x00" * 16]
    )
    assert bad.is_yubikey is False
    assert any("aaguid" in r.lower() for r in bad.reasons)


def test_verify_attestation_object_can_skip_root_pinning():
    ch = Chain()
    att_obj, cdh = ch.attestation()
    # Real Yubico roots won't match a throwaway CA; without pinning the vendor
    # name + signature checks still pass.
    check = yubikey.verify_attestation_object(att_obj, cdh, require_root=False)
    assert check.is_yubikey is True
    assert check.trusted_root_subject is None


def test_verify_attestation_object_against_real_yubico_roots_rejects_fake():
    ch = Chain()
    att_obj, cdh = ch.attestation()
    check = yubikey.verify_attestation_object(att_obj, cdh)  # defaults to bundled roots
    assert check.is_yubikey is False


def test_verify_attestation_object_reports_none_format_as_not_attested():
    auth_data = AuthenticatorData.create(
        b"\x22" * 32,
        AuthenticatorData.FLAG.AT | AuthenticatorData.FLAG.UP,
        0,
        AttestedCredentialData.create(
            AAGUID,
            b"cred-id",
            ES256.from_cryptography_key(ec.generate_private_key(ec.SECP256R1()).public_key()),
        ),
    )
    att_obj = AttestationObject.create("none", auth_data, {})
    check = yubikey.verify_attestation_object(att_obj, b"\x11" * 32)
    assert check.is_yubikey is False
    assert check.fmt == "none"
    assert check.reasons


def test_verify_yubikey_attestation_uses_the_result_fields():
    ch = Chain()
    att_obj, cdh = ch.attestation()
    result = make_result(att_obj=att_obj, client_data_hash=cdh)
    check = yubikey.verify_yubikey_attestation(result, roots=[_der(ch.root)])
    assert check.is_yubikey is True


def test_verify_yubikey_attestation_without_attestation_object():
    result = make_result(att_obj=None)
    with pytest.raises(yubikey.AttestationError):
        yubikey.verify_yubikey_attestation(result)
