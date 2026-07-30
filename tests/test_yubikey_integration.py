"""Integration tests against a real YubiKey (CLAUDE.md §8.6). Optional.

Three tiers, each skipping on its own:

* ``requires_yubikey``      — a key is plugged in. Read-only, no PIN, no touch.
* ``requires_preview_sign`` — that key advertises previewSign (firmware 5.8.0+).
* ``requires_yubikey_touch``— you agreed to press the button: set
  ``AI_REMOTE_YUBIKEY_TOUCH=1``. Without it these are skipped so an unattended
  ``py -m pytest`` never blocks on a finger.

Run the whole thing (Windows: **administrator** terminal, else the native WebAuthn
path drops the previewSign output):

    $env:AI_REMOTE_YUBIKEY_TOUCH="1"; py -m pytest tests/test_yubikey_integration.py -v -s

``-s`` matters: without it pytest swallows the "touch your YubiKey now" prompt.
"""
import pytest

pytest.importorskip("fido2", reason="optional extra not installed: uv sync --extra yubikey")

from fido2.cose import ESP256, ARKG_P256_PLACEHOLDER  # noqa: E402

from lib import yubikey  # noqa: E402
from tests.conftest import (  # noqa: E402
    requires_preview_sign,
    requires_yubikey,
    requires_yubikey_touch,
    run_async,
)


# --- tier 1: no touch, no PIN --------------------------------------------------
@requires_yubikey
def test_device_is_enumerable():
    assert list(yubikey.enumerate_devices())


@requires_yubikey
def test_get_version_reports_a_firmware():
    version = yubikey.get_version()
    assert version.is_known, "device reported no firmwareVersion"
    assert version.major >= 4, f"implausible firmware {version}"
    print(f"\nfirmware: {version}")


@requires_yubikey
def test_get_device_info_is_coherent():
    info = yubikey.get_device_info()
    assert len(info.aaguid) == 16
    assert info.versions, "device advertises no CTAP versions"
    # meets_arkg_firmware must agree with the decoded version, not be independent.
    assert info.meets_arkg_firmware == (
        info.firmware_version.is_known and info.firmware_version >= yubikey.MIN_ARKG_FIRMWARE
    )
    print(f"\naaguid={info.aaguid_hex} versions={info.versions} previewSign={info.supports_preview_sign}")


@requires_yubikey
def test_preview_sign_support_matches_firmware_expectation():
    info = yubikey.get_device_info()
    if info.supports_preview_sign:
        # A key that offers ARKG should be on 5.8.0+; flag the surprise loudly.
        assert info.meets_arkg_firmware, (
            f"previewSign advertised on firmware {info.firmware_version}, below "
            f"{yubikey.MIN_ARKG_FIRMWARE} — worth reporting upstream"
        )
    else:
        pytest.skip(f"previewSign not advertised (firmware {info.firmware_version})")


# --- tier 3: needs a button press ---------------------------------------------
@pytest.fixture(scope="module")
def live_credential():
    """One real makeCredential(previewSign) shared by the tests below (one touch)."""
    print("\n\n=== makeCredential: touch your YubiKey when it blinks ===")
    return yubikey.make_credential()


@requires_yubikey
@requires_preview_sign
@requires_yubikey_touch
def test_make_credential_returns_an_arkg_seed_key(live_credential):
    result = live_credential
    assert result.key_handle, "no key handle returned"
    assert result.seed_public_key_cbor, "no seed public key returned"
    assert len(result.aaguid) == 16
    assert len(result.client_data_hash) == 32

    # The seed key must really be an ARKG-P256 key, not a plain credential key.
    seed = yubikey.parse_seed_public_key(result.seed_public_key_cbor)
    assert isinstance(seed, ARKG_P256_PLACEHOLDER)
    print(f"\nkey_handle={result.key_handle.hex()} aaguid={result.aaguid_hex}")


@requires_yubikey
@requires_preview_sign
@requires_yubikey_touch
def test_seed_public_key_derives_offline_from_a_real_seed(live_credential):
    derived = yubikey.seed_public_key(live_credential, ctx=b"integration-test")

    assert isinstance(derived.derived_public_key, ESP256)
    assert len(derived.ikm) == 32
    assert derived.arkg_args[-2] == b"integration-test"
    assert derived.arkg_args[-1], "no key handle blob in the COSE_Sign_Args"
    print(f"\nderived x={bytes(derived.derived_public_key[-2]).hex()}")


@requires_yubikey
@requires_preview_sign
@requires_yubikey_touch
def test_real_seed_key_derivation_is_deterministic_and_unlinkable(live_credential):
    ikm = b"\x42" * 32
    a = yubikey.seed_public_key(live_credential, ctx=b"ctx-a", ikm=ikm)
    again = yubikey.seed_public_key(live_credential, ctx=b"ctx-a", ikm=ikm)
    other_ctx = yubikey.seed_public_key(live_credential, ctx=b"ctx-b", ikm=ikm)
    other_ikm = yubikey.seed_public_key(live_credential, ctx=b"ctx-a", ikm=b"\x43" * 32)

    assert a.derived_public_key == again.derived_public_key
    assert a.derived_public_key != other_ctx.derived_public_key
    assert a.derived_public_key != other_ikm.derived_public_key


@requires_yubikey
@requires_preview_sign
@requires_yubikey_touch
def test_real_credential_survives_a_save_load_round_trip(live_credential, tmp_path):
    path = tmp_path / "cred.json"
    yubikey.save_result(live_credential, path)
    restored = yubikey.load_result(path)

    ikm = b"\x11" * 32
    assert (
        yubikey.seed_public_key(restored, ctx=b"ctx", ikm=ikm).derived_public_key
        == yubikey.seed_public_key(live_credential, ctx=b"ctx", ikm=ikm).derived_public_key
    )


@requires_yubikey
@requires_preview_sign
@requires_yubikey_touch
def test_real_attestation_identifies_a_genuine_yubikey(live_credential):
    """The payoff: a real key must verify against the bundled Yubico roots."""
    check = yubikey.verify_yubikey_attestation(live_credential)
    print(f"\nsubject     : {check.certificate_subject}")
    print(f"trusted root: {check.trusted_root_subject}")
    print(f"aaguid      : {check.aaguid_hex}")
    for reason in check.reasons:
        print(f"  ! {reason}")

    assert check.attestation_verified, f"attestation statement failed: {check.reasons}"
    assert check.is_yubikey, f"not identified as a YubiKey: {check.reasons}"
    assert check.trusted_root_subject, "chain did not pin to a Yubico root"


@requires_yubikey
@requires_preview_sign
@requires_yubikey_touch
def test_real_attestation_rejected_against_a_foreign_root(live_credential):
    """Sanity check on the pinning: a genuine key must fail against a wrong root."""
    import datetime

    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Not A Yubico Root")])
    now = datetime.datetime(2025, 1, 1, tzinfo=datetime.timezone.utc)
    foreign = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(now + datetime.timedelta(days=365))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )

    check = yubikey.verify_yubikey_attestation(
        live_credential, roots=[foreign.public_bytes(serialization.Encoding.DER)]
    )
    assert check.is_yubikey is False
    assert check.attestation_verified is True, "statement itself should still verify"


@requires_yubikey
@requires_preview_sign
@requires_yubikey_touch
def test_sign_with_derived_key_round_trips(live_credential):
    """The full ARKG point: the device signs with a key only it can reconstruct, and
    the offline-derived public key verifies that signature."""
    import hashlib

    derived = yubikey.seed_public_key(live_credential, ctx=b"sign-test")
    message = b"ai-remote integration message"

    print("\n\n=== signing: touch your YubiKey again when it blinks ===")
    signature = yubikey.sign_with_derived_key(live_credential, derived, data=message)
    print(f"\nsignature: {signature.hex()}")

    assert signature, "no signature returned"
    assert yubikey.verify_signature(derived, signature, data=message) is True
    # The digest path must accept the very same signature.
    assert yubikey.verify_signature(
        derived, signature, digest=hashlib.sha256(message).digest()
    ) is True
    # ... and must reject a different message and a foreign key.
    assert yubikey.verify_signature(derived, signature, data=b"other message") is False
    other = yubikey.seed_public_key(live_credential, ctx=b"sign-test", ikm=b"\x99" * 32)
    assert yubikey.verify_signature(other, signature, data=message) is False


@requires_yubikey
@requires_preview_sign
@requires_yubikey_touch
def test_sign_accepts_a_precomputed_digest(live_credential):
    """Signing a bare hash (no original data) must verify the same way."""
    import hashlib

    derived = yubikey.seed_public_key(live_credential, ctx=b"digest-test")
    message = b"only the hash is at hand"
    digest = hashlib.sha256(message).digest()

    print("\n\n=== signing a digest: touch your YubiKey ===")
    signature = yubikey.sign_with_derived_key(live_credential, derived, digest=digest)

    assert yubikey.verify_signature(derived, signature, digest=digest) is True
    assert yubikey.verify_signature(derived, signature, data=message) is True


@requires_yubikey
@requires_preview_sign
@requires_yubikey_touch
def test_yubikey_exec_run_end_to_end(tmp_path, capsys):
    """The utility itself, against real hardware: version -> make-credential -> derive."""
    from tools import yubikey_exec

    assert yubikey_exec.main(["version"]) == yubikey_exec.EXIT_OK

    out = tmp_path / "cred.json"
    print("\n\n=== yubikey-exec run: touch your YubiKey when it blinks ===")
    with capsys.disabled():
        code = yubikey_exec.main(
            ["run", "--out", str(out), "--ctx", "exec-integration", "--require-yubikey"]
        )
    assert code == yubikey_exec.EXIT_OK, "run --require-yubikey did not verify the key"
    assert out.is_file()

    # And the saved file must derive again with no device involved.
    assert yubikey_exec.main(["derive", "--in", str(out)]) == yubikey_exec.EXIT_OK


@requires_yubikey
@requires_preview_sign
@requires_yubikey_touch
def test_yubikey_responder_reply_is_accepted_by_the_hook(live_credential, capsys):
    """The whole §7 chain on real silicon: the YubiKey signs a decision, hook.py trusts it.

    This is the round trip nothing else can cover — the ARKG private half exists only
    inside the authenticator (§8.4), so no offline stand-in proves that what the device
    actually emits passes ``hook.verify_reply``.
    """
    from approver import hook, responder, responder_yubikey
    from lib import crypto

    # What `register` would have produced, minus the bus.
    derived = yubikey.seed_public_key(live_credential, ctx=b"approvals-integration")
    cfg = responder_yubikey.config_from_derivation("approver-yk", live_credential, derived)
    assert cfg["key_type"] == crypto.P256

    # ... and what `serve` rebuilds from that config alone, with no device involved.
    result, restored = responder_yubikey.derived_from_config(cfg)

    payload = {
        "hook_event_name": "PermissionRequest",
        "session_id": "yubikey-integration",
        "tool_name": "Bash",
        "tool_input": {"command": "echo hello"},
        "permission_mode": "default",
        "cwd": ".",
    }
    request = hook.build_request(payload, nonce="bm9uY2UtaW50ZWdyYXRpb24=", ts=1737345600)
    handler = responder.make_approval_handler(
        key_id="approver-yk",
        sign=responder_yubikey.device_signer(
            result, restored, user_interaction=yubikey.console_user_interaction()
        ),
        prompt=lambda req: ("allow", "approved on the key", None),
    )

    print("\n\n=== signing an approval decision: touch your YubiKey ===")
    with capsys.disabled():
        reply = run_async(handler(request))

    assert reply is not None, "the responder produced no reply (missed touch?)"
    allowlist = {"approver-yk": {"pubkey": cfg["public_key"], "key_type": crypto.P256}}
    assert hook.verify_reply(request, reply, allowlist) == (True, "ok")
    assert hook.decision_output(reply)["hookSpecificOutput"]["decision"]["behavior"] == "allow"

    # Same signature, flipped decision — the hook must reject it (no extra touch).
    reply["behavior"] = "deny"
    assert hook.verify_reply(request, reply, allowlist)[0] is False


@requires_yubikey
@requires_preview_sign
@requires_yubikey_touch
def test_yubikey_exec_sign_then_verify_offline(tmp_path, capsys):
    """`sign` on hardware, then `verify` with no device — the split the CLI promises."""
    import json as _json

    from tools import yubikey_exec

    out = tmp_path / "cred.json"
    print("\n\n=== yubikey-exec make-credential: touch your YubiKey ===")
    with capsys.disabled():
        assert yubikey_exec.main(["make-credential", "--out", str(out)]) == yubikey_exec.EXIT_OK

    ikm = "5a" * 32  # fixed, so `verify` can reproduce the same derived key
    print("\n=== yubikey-exec sign: touch your YubiKey again ===")
    with capsys.disabled():
        code = yubikey_exec.main(
            ["--json", "sign", "--in", str(out), "--ikm", ikm,
             "--ctx", "cli-sign", "--message", "hello from the cmd line"]
        )
    assert code == yubikey_exec.EXIT_OK
    signature = _json.loads(capsys.readouterr().out)["sign"]["signature"]

    # Offline verification of that signature.
    assert yubikey_exec.main(
        ["verify", "--in", str(out), "--ikm", ikm, "--ctx", "cli-sign",
         "--message", "hello from the cmd line", "--signature", signature]
    ) == yubikey_exec.EXIT_OK
    # A tampered message must be rejected.
    assert yubikey_exec.main(
        ["verify", "--in", str(out), "--ikm", ikm, "--ctx", "cli-sign",
         "--message", "tampered", "--signature", signature]
    ) == yubikey_exec.EXIT_BAD_SIGNATURE
