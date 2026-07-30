"""Tests for the yubikey-exec utility (tools/yubikey_exec.py, CLAUDE.md §8.5).

No hardware: `derive` reads a saved credential and needs no device at all, so the
whole subcommand is covered here. Device-touching subcommands are covered by their
argument parsing plus the error path when no key is present; the real hardware runs
live in test_yubikey_integration.py.
"""
import json

import pytest

pytest.importorskip("fido2", reason="optional extra not installed: uv sync --extra yubikey")

from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402
from fido2 import cbor  # noqa: E402
from fido2.cose import ARKG_P256_PLACEHOLDER, ES256, ESP256, ESP256_SPLIT_ARKG_PLACEHOLDER  # noqa: E402

from lib import yubikey  # noqa: E402
from tools import yubikey_exec  # noqa: E402


# --- fixtures ------------------------------------------------------------------
def _cose_ec2(pub):
    n = pub.public_numbers()
    return {1: 2, 3: ES256.ALGORITHM, -1: 1, -2: n.x.to_bytes(32, "big"), -3: n.y.to_bytes(32, "big")}


def _seed_cbor():
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


@pytest.fixture
def saved_credential(tmp_path):
    """A make-credential result on disk, as `--out` would have written it."""
    result = yubikey.MakeCredentialResult(
        key_handle=bytes.fromhex("aabbccdd"),
        seed_public_key_cbor=_seed_cbor(),
        algorithm=ESP256_SPLIT_ARKG_PLACEHOLDER,
        credential_id=bytes.fromhex("0011223344"),
        aaguid=bytes.fromhex("d7781e5de35346aaafe23ca49f13332a"),
        client_data_hash=b"\x11" * 32,
    )
    path = tmp_path / "cred.json"
    yubikey.save_result(result, path)
    return path


# --- pure helpers --------------------------------------------------------------
def test_parse_ikm_none_means_generate():
    assert yubikey_exec.parse_ikm(None) is None


def test_parse_ikm_decodes_hex():
    assert yubikey_exec.parse_ikm("00ff") == b"\x00\xff"


def test_parse_ikm_rejects_non_hex():
    with pytest.raises(yubikey.YubiKeyError):
        yubikey_exec.parse_ikm("zz")


def test_device_report_is_json_safe():
    info = yubikey.device_info_from_info(
        type("I", (), {"extensions": ["previewSign"], "firmware_version": 0x050800,
                       "aaguid": b"\xab" * 16, "versions": ["FIDO_2_1"]})()
    )
    report = yubikey_exec.device_report(info)
    json.dumps(report)
    assert report["firmware_version"] == "5.8.0"
    assert report["preview_sign"] is True
    assert report["meets_arkg_firmware"] is True


# --- argument parsing ----------------------------------------------------------
def test_subcommand_is_required():
    with pytest.raises(SystemExit):
        yubikey_exec._build_parser().parse_args([])


@pytest.mark.parametrize("cmd", ["version", "make-credential", "run"])
def test_attestation_check_is_off_by_default(cmd):
    args = yubikey_exec._build_parser().parse_args([cmd])
    # `version` has no attest flags at all; the others default to not checking.
    assert getattr(args, "attest", False) is False
    assert getattr(args, "require_yubikey", False) is False


def test_attest_flag_parses():
    args = yubikey_exec._build_parser().parse_args(["run", "--attest"])
    assert args.attest is True
    assert args.require_yubikey is False


def test_require_yubikey_flag_parses():
    args = yubikey_exec._build_parser().parse_args(["run", "--require-yubikey"])
    assert args.require_yubikey is True


def test_derive_requires_input():
    with pytest.raises(SystemExit):
        yubikey_exec._build_parser().parse_args(["derive"])


def test_derive_defaults_ctx():
    args = yubikey_exec._build_parser().parse_args(["derive", "--in", "x.json"])
    assert args.ctx == yubikey_exec.DEFAULT_CTX


# --- derive: the offline half, end to end through main() -----------------------
def test_derive_runs_without_a_device(saved_credential, capsys):
    code = yubikey_exec.main(["derive", "--in", str(saved_credential)])
    assert code == yubikey_exec.EXIT_OK
    out = capsys.readouterr().out
    assert "derived key" in out
    assert "arkg_args" in out


def test_derive_json_output_is_one_document(saved_credential, capsys):
    code = yubikey_exec.main(["--json", "derive", "--in", str(saved_credential)])
    assert code == yubikey_exec.EXIT_OK
    payload = json.loads(capsys.readouterr().out)
    assert payload["credential"]["key_handle"] == "aabbccdd"
    assert payload["derived"]["arkg_args"]["algorithm"] == ESP256_SPLIT_ARKG_PLACEHOLDER
    assert len(bytes.fromhex(payload["derived"]["ikm"])) == 32
    assert payload["derived"]["derived_public_key"]["algorithm"] == ESP256.ALGORITHM


def test_derive_with_explicit_ikm_is_reproducible(saved_credential, capsys):
    ikm = "07" * 32
    yubikey_exec.main(["--json", "derive", "--in", str(saved_credential), "--ikm", ikm])
    first = json.loads(capsys.readouterr().out)
    yubikey_exec.main(["--json", "derive", "--in", str(saved_credential), "--ikm", ikm])
    second = json.loads(capsys.readouterr().out)
    assert first["derived"]["derived_public_key"] == second["derived"]["derived_public_key"]
    assert first["derived"]["arkg_args"] == second["derived"]["arkg_args"]


def test_derive_different_ctx_changes_the_key(saved_credential, capsys):
    ikm = "07" * 32
    yubikey_exec.main(["--json", "derive", "--in", str(saved_credential), "--ikm", ikm, "--ctx", "a"])
    a = json.loads(capsys.readouterr().out)
    yubikey_exec.main(["--json", "derive", "--in", str(saved_credential), "--ikm", ikm, "--ctx", "b"])
    b = json.loads(capsys.readouterr().out)
    assert a["derived"]["derived_public_key"]["x"] != b["derived"]["derived_public_key"]["x"]


def test_derive_rejects_short_ikm(saved_credential, capsys):
    code = yubikey_exec.main(["derive", "--in", str(saved_credential), "--ikm", "0102"])
    assert code == yubikey_exec.EXIT_ERROR
    assert "ikm must be at least" in capsys.readouterr().err


def test_derive_reports_a_missing_file(tmp_path, capsys):
    code = yubikey_exec.main(["derive", "--in", str(tmp_path / "nope.json")])
    assert code == yubikey_exec.EXIT_ERROR
    assert "cannot read saved credential" in capsys.readouterr().err


def test_derive_skips_attestation_when_credential_has_none(saved_credential, capsys):
    # The saved credential carries no attestation object; --attest must not crash.
    code = yubikey_exec.main(["--json", "derive", "--in", str(saved_credential), "--attest"])
    assert code == yubikey_exec.EXIT_OK
    assert "attestation" not in json.loads(capsys.readouterr().out)


# --- sign / verify argument handling ------------------------------------------
def test_sign_requires_input_and_message():
    parser = yubikey_exec._build_parser()
    with pytest.raises(SystemExit):
        parser.parse_args(["sign", "--in", "x.json"])  # no --message/--digest
    with pytest.raises(SystemExit):
        parser.parse_args(["sign", "--message", "hi"])  # no --in


def test_sign_accepts_a_message():
    args = yubikey_exec._build_parser().parse_args(
        ["sign", "--in", "x.json", "--message", "hello"]
    )
    assert args.message == "hello"
    assert args.digest is None


def test_sign_accepts_a_digest_instead_of_a_message():
    args = yubikey_exec._build_parser().parse_args(
        ["sign", "--in", "x.json", "--digest", "ab" * 32]
    )
    assert args.digest == "ab" * 32
    assert args.message is None


def test_message_and_digest_are_mutually_exclusive():
    with pytest.raises(SystemExit):
        yubikey_exec._build_parser().parse_args(
            ["sign", "--in", "x.json", "--message", "hi", "--digest", "ab" * 32]
        )


def test_signing_payload_from_message():
    import hashlib

    args = yubikey_exec._build_parser().parse_args(
        ["sign", "--in", "x.json", "--message", "hello"]
    )
    data, digest = yubikey_exec.signing_payload(args)
    assert data == b"hello"
    assert digest is None
    # The library hashes it; check the CLI hands over the right bytes.
    assert yubikey.signing_digest(data=data) == hashlib.sha256(b"hello").digest()


def test_signing_payload_from_digest():
    args = yubikey_exec._build_parser().parse_args(
        ["sign", "--in", "x.json", "--digest", "ab" * 32]
    )
    data, digest = yubikey_exec.signing_payload(args)
    assert data is None
    assert digest == bytes.fromhex("ab" * 32)


def test_signing_payload_rejects_non_hex_digest():
    args = yubikey_exec._build_parser().parse_args(
        ["sign", "--in", "x.json", "--digest", "zz"]
    )
    with pytest.raises(yubikey.YubiKeyError):
        yubikey_exec.signing_payload(args)


# --- verify: offline, no device ------------------------------------------------
def _esp256_signed(message):
    """A derived-key stand-in plus a real signature over ``message``."""
    from cryptography.hazmat.primitives import hashes
    from fido2.cose import ESP256

    priv = ec.generate_private_key(ec.SECP256R1())
    pub = ESP256.from_cryptography_key(priv.public_key())
    sig = priv.sign(message, ec.ECDSA(hashes.SHA256()))
    return pub, sig


def test_verify_subcommand_accepts_a_valid_signature(tmp_path, capsys, monkeypatch):
    pub, sig = _esp256_signed(b"hello")
    # The public key normally comes from re-deriving; short-circuit that so this test
    # stays offline and focused on the verify path.
    monkeypatch.setattr(
        yubikey_exec, "_derived_public_key_for_verify", lambda args: (pub, None)
    )
    code = yubikey_exec.main(
        ["--json", "verify", "--in", str(tmp_path / "unused.json"),
         "--message", "hello", "--signature", sig.hex()]
    )
    assert code == yubikey_exec.EXIT_OK
    assert json.loads(capsys.readouterr().out)["verify"]["verified"] is True


def test_verify_subcommand_rejects_a_tampered_message(tmp_path, capsys, monkeypatch):
    pub, sig = _esp256_signed(b"hello")
    monkeypatch.setattr(
        yubikey_exec, "_derived_public_key_for_verify", lambda args: (pub, None)
    )
    code = yubikey_exec.main(
        ["--json", "verify", "--in", str(tmp_path / "unused.json"),
         "--message", "tampered", "--signature", sig.hex()]
    )
    assert code == yubikey_exec.EXIT_BAD_SIGNATURE
    assert json.loads(capsys.readouterr().out)["verify"]["verified"] is False


def test_verify_subcommand_rejects_non_hex_signature(saved_credential, capsys):
    code = yubikey_exec.main(
        ["verify", "--in", str(saved_credential), "--message", "hi", "--signature", "zz"]
    )
    assert code == yubikey_exec.EXIT_ERROR
    assert "hex" in capsys.readouterr().err


def test_verify_end_to_end_rederives_the_key_from_the_credential(saved_credential, capsys):
    """No monkeypatching: --ikm reproduces the same derived key, so verify can run
    entirely offline against a saved credential."""
    ikm = "07" * 32
    yubikey_exec.main(["--json", "derive", "--in", str(saved_credential), "--ikm", ikm])
    derived = json.loads(capsys.readouterr().out)["derived"]

    # A signature the real device would make is unavailable (no derived private key),
    # so assert the negative: a bogus signature must be reported as not verified.
    code = yubikey_exec.main(
        ["--json", "verify", "--in", str(saved_credential), "--ikm", ikm,
         "--message", "hello", "--signature", "00" * 70]
    )
    assert code == yubikey_exec.EXIT_BAD_SIGNATURE
    payload = json.loads(capsys.readouterr().out)
    assert payload["verify"]["verified"] is False
    # ... and that it re-derived the very same key it would have signed with.
    assert payload["verify"]["derived_public_key"]["x"] == derived["derived_public_key"]["x"]


def test_verify_requires_ikm_to_reproduce_the_key(saved_credential, capsys):
    code = yubikey_exec.main(
        ["verify", "--in", str(saved_credential), "--message", "hi", "--signature", "00" * 70]
    )
    assert code == yubikey_exec.EXIT_ERROR
    assert "--ikm" in capsys.readouterr().err


# --- error paths that do not need hardware ------------------------------------
def test_version_without_a_device_errors_cleanly(monkeypatch, capsys):
    monkeypatch.setattr(yubikey, "get_device_info", lambda **kw: (_ for _ in ()).throw(
        yubikey.NoAuthenticator("no FIDO authenticator found")
    ))
    code = yubikey_exec.main(["version"])
    assert code == yubikey_exec.EXIT_ERROR
    assert "no FIDO authenticator found" in capsys.readouterr().err
