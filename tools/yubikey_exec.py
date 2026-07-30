"""yubikey-exec — command-line front end for lib/yubikey.py (CLAUDE.md §8.5).

(The utility is called ``yubikey-exec``; the file uses an underscore so the module
stays importable and unit-testable, same convention as
``approver/registration_handler.py``.)

Four subcommands, mirroring the library's four entry points:

  version           Firmware version, AAGUID, previewSign support. No touch.

  make-credential   makeCredential(previewSign) -> an ARKG seed key on the device.
                    Needs a touch. ``--out FILE`` saves the result so `derive` can
                    run later, in a separate process, without the key.

  derive            derive_public_key on its own: reads a saved credential and
                    derives derived_public_key + arkg_args. No device, no touch.

  run               make-credential and derive together in one invocation.

The "is this really a YubiKey?" attestation check is **optional and off by default**
everywhere: pass ``--attest`` to run and report it, and ``--require-yubikey`` to
additionally make a negative verdict a non-zero exit.

Exit codes: 0 = ok, 1 = error (no key, no fido2, bad input), 2 = attestation says
this is not a YubiKey and ``--require-yubikey`` was given.

Run with the `py` launcher (CLAUDE.md §5):
  py tools/yubikey_exec.py version
  py tools/yubikey_exec.py run --ctx my-purpose --attest
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Non-package project: make repo-root imports work when run directly as a script.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from lib import yubikey  # noqa: E402

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_NOT_YUBIKEY = 2
EXIT_BAD_SIGNATURE = 3

DEFAULT_CTX = "ai-remote"


# --- pure helpers --------------------------------------------------------------
def parse_ikm(text: str | None) -> bytes | None:
    """Decode a hex ``--ikm`` argument, or None to let the library generate one.

    Explicit ikm exists for reproducing a derivation; it is validated by
    ``lib.yubikey.validate_ikm`` (>= 32 bytes) once it reaches the library.
    """
    if text is None:
        return None
    try:
        return bytes.fromhex(text)
    except ValueError as e:
        raise yubikey.YubiKeyError(f"--ikm must be hex: {e}") from e


def device_report(info: yubikey.DeviceInfo) -> dict:
    """JSON-safe summary of a :class:`lib.yubikey.DeviceInfo`."""
    return {
        "firmware_version": str(info.firmware_version),
        "firmware_version_known": info.firmware_version.is_known,
        "aaguid": info.aaguid_hex,
        "ctap_versions": list(info.versions),
        "extensions": list(info.extensions),
        "preview_sign": info.supports_preview_sign,
        "meets_arkg_firmware": info.meets_arkg_firmware,
        "min_arkg_firmware": str(yubikey.MIN_ARKG_FIRMWARE),
    }


def credential_report(result: yubikey.MakeCredentialResult) -> dict:
    """JSON-safe summary of a makeCredential result (no secrets: none exist here)."""
    return {
        "key_handle": result.key_handle.hex(),
        "credential_id": result.credential_id.hex(),
        "aaguid": result.aaguid_hex,
        "seed_key_algorithm": result.algorithm,
        "has_attestation": result.attestation_object is not None,
        "has_seed_attestation": result.seed_attestation_object is not None,
    }


def public_key_report(key) -> dict:
    """JSON-safe summary of a COSE EC public key (or a DerivedKey wrapping one)."""
    pk = getattr(key, "derived_public_key", key)
    from fido2 import cbor

    return {
        "algorithm": pk.get(3),
        "x": bytes(pk[-2]).hex(),
        "y": bytes(pk[-3]).hex(),
        "cose_cbor": cbor.encode(dict(pk)).hex(),
    }


def derived_report(derived: yubikey.DerivedKey) -> dict:
    """JSON-safe summary of a derived ARKG key plus the args needed to sign."""
    args = dict(derived.arkg_args)
    return {
        "ctx": derived.ctx.decode("utf-8", "replace"),
        "ikm": derived.ikm.hex(),
        "key_handle": derived.key_handle.hex(),
        "derived_public_key": public_key_report(derived),
        "arkg_args": {
            "algorithm": args.get(3),
            "key_handle_blob": bytes(args[-1]).hex(),
            "ctx": bytes(args[-2]).hex(),
            "cbor": derived.arkg_args_cbor.hex(),
        },
    }


def signing_payload(args) -> tuple[bytes | None, bytes | None]:
    """Turn ``--message`` / ``--digest`` into the ``(data, digest)`` pair the library takes.

    Exactly one is non-None; the parser already enforces that one was given.
    """
    if getattr(args, "digest", None):
        try:
            digest = bytes.fromhex(args.digest)
        except ValueError as e:
            raise yubikey.YubiKeyError(f"--digest must be hex: {e}") from e
        return None, digest
    return args.message.encode("utf-8"), None


def parse_signature(text: str) -> bytes:
    try:
        return bytes.fromhex(text)
    except ValueError as e:
        raise yubikey.YubiKeyError(f"--signature must be hex: {e}") from e


def signature_report(signature: bytes, *, verified: bool | None = None) -> dict:
    report: dict = {"signature": signature.hex(), "signature_bytes": len(signature)}
    if verified is not None:
        report["verified"] = verified
    return report


def attestation_report(check: yubikey.AttestationCheck) -> dict:
    """JSON-safe summary of the optional YubiKey-genuineness verdict."""
    return {
        "is_yubikey": check.is_yubikey,
        "attestation_verified": check.attestation_verified,
        "format": check.fmt,
        "attestation_type": check.attestation_type,
        "aaguid": check.aaguid_hex,
        "certificate_subject": check.certificate_subject,
        "certificate_issuer": check.certificate_issuer,
        "trusted_root_subject": check.trusted_root_subject,
        "chain_length": check.chain_length,
        "reasons": list(check.reasons),
    }


# --- console interaction -------------------------------------------------------
def _user_interaction():
    """A fido2 UserInteraction that prompts on the console for touch / PIN."""
    from fido2.client import UserInteraction

    class _Cli(UserInteraction):
        def prompt_up(self):
            print("\n>>> touch your YubiKey now <<<\n", file=sys.stderr)

        def request_pin(self, permissions, rd_id):
            from getpass import getpass

            return getpass("enter YubiKey PIN: ")

        def request_uv(self, permissions, rd_id):
            print("user verification required", file=sys.stderr)
            return True

    return _Cli()


def _print(report: dict, *, as_json: bool, title: str | None = None) -> None:
    if as_json:
        return  # JSON mode prints one document at the end instead
    if title:
        print(f"\n=== {title} ===")
    width = max((len(k) for k in report), default=0)
    for key, value in report.items():
        if isinstance(value, dict):
            print(f"{key}:")
            inner = max((len(k) for k in value), default=0)
            for k2, v2 in value.items():
                print(f"  {k2.ljust(inner)} : {v2}")
        elif isinstance(value, list):
            print(f"{key.ljust(width)} : {', '.join(str(v) for v in value) or '-'}")
        else:
            print(f"{key.ljust(width)} : {value}")


# --- attestation (optional everywhere) -----------------------------------------
def _maybe_attest(result, args, out: dict) -> int:
    """Run the optional YubiKey check. Returns the exit code contribution."""
    if not (args.attest or args.require_yubikey):
        return EXIT_OK

    roots = yubikey.load_yubico_roots(args.roots) if args.roots else None
    check = yubikey.verify_yubikey_attestation(
        result,
        roots=roots,
        require_root=not args.no_require_root,
        use_seed_attestation=args.seed_attestation,
    )
    report = attestation_report(check)
    out["attestation"] = report
    _print(report, as_json=args.json, title="attestation")

    if check.is_yubikey:
        return EXIT_OK
    if not args.json:
        print("\nnot verified as a YubiKey:", file=sys.stderr)
        for reason in check.reasons:
            print(f"  ! {reason}", file=sys.stderr)
    return EXIT_NOT_YUBIKEY if args.require_yubikey else EXIT_OK


# --- subcommands ---------------------------------------------------------------
def cmd_version(args) -> int:
    info = yubikey.get_device_info()
    report = device_report(info)
    _print(report, as_json=args.json, title="device")
    if args.json:
        print(json.dumps({"device": report}, indent=2))
    return EXIT_OK


def cmd_make_credential(args) -> int:
    out: dict = {}
    result = _do_make_credential(args, out)
    code = _maybe_attest(result, args, out)
    if args.json:
        print(json.dumps(out, indent=2))
    return code


def cmd_derive(args) -> int:
    out: dict = {}
    result = yubikey.load_result(args.input)
    out["credential"] = credential_report(result)
    _print(out["credential"], as_json=args.json, title=f"credential ({args.input})")

    derived = _do_derive(args, result, out)
    code = _maybe_attest(result, args, out) if result.attestation_object else EXIT_OK
    if args.json:
        print(json.dumps(out, indent=2))
    del derived
    return code


def cmd_run(args) -> int:
    out: dict = {}
    result = _do_make_credential(args, out)
    derived = _do_derive(args, result, out)
    code = _maybe_attest(result, args, out)

    # Signing is opt-in on `run`: only when something to sign was given.
    if args.message is not None or args.digest is not None:
        sign_code = _do_sign(args, result, derived, out)
        code = code or sign_code

    if args.json:
        print(json.dumps(out, indent=2))
    return code


def _do_sign(args, result, derived, out: dict) -> int:
    """Sign with ``derived`` and immediately verify. Returns the exit code."""
    data, digest = signing_payload(args)
    if not args.json:
        print("\n>>> signing needs a touch <<<")

    signature = yubikey.sign_with_derived_key(
        result,
        derived,
        data=data,
        digest=digest,
        rp_id=args.rp_id,
        user_verification=args.user_verification,
        user_interaction=_user_interaction(),
    )
    # Verify straight away: a signature the derived public key cannot check is useless,
    # and this is the only point where both halves are in hand.
    verified = yubikey.verify_signature(derived, signature, data=data, digest=digest)
    out["sign"] = signature_report(signature, verified=verified)
    _print(out["sign"], as_json=args.json, title="signature")
    if not verified and not args.json:
        print("\nWARNING: the signature does not verify against the derived key", file=sys.stderr)
    return EXIT_OK if verified else EXIT_BAD_SIGNATURE


def cmd_sign(args) -> int:
    out: dict = {}
    result = yubikey.load_result(args.input)
    out["credential"] = credential_report(result)
    _print(out["credential"], as_json=args.json, title=f"credential ({args.input})")

    derived = _do_derive(args, result, out)
    code = _do_sign(args, result, derived, out)
    if args.json:
        print(json.dumps(out, indent=2))
    return code


def _derived_public_key_for_verify(args):
    """Re-derive the public key a signature should be checked against.

    Verification is offline, so the key has to be reproduced from the saved credential
    plus the original ``ctx``/``ikm`` — which is why ``--ikm`` is required here.
    """
    if not args.ikm:
        raise yubikey.YubiKeyError(
            "--ikm is required to verify: the derived key can only be reproduced from "
            "the same ikm and ctx that produced it (sign prints the ikm it used)"
        )
    result = yubikey.load_result(args.input)
    derived = yubikey.seed_public_key(
        result, ctx=args.ctx.encode("utf-8"), ikm=parse_ikm(args.ikm)
    )
    return derived, derived_report(derived)


def cmd_verify(args) -> int:
    out: dict = {}
    signature = parse_signature(args.signature)
    data, digest = signing_payload(args)

    key, _report = _derived_public_key_for_verify(args)
    verified = yubikey.verify_signature(key, signature, data=data, digest=digest)

    out["verify"] = {
        **signature_report(signature, verified=verified),
        "ctx": args.ctx,
        "derived_public_key": public_key_report(key),
    }
    _print(
        {k: v for k, v in out["verify"].items() if k != "derived_public_key"},
        as_json=args.json,
        title="verify",
    )
    if not args.json:
        print(f"\n{'OK: signature verified' if verified else 'FAIL: signature did NOT verify'}")
    else:
        print(json.dumps(out, indent=2))
    return EXIT_OK if verified else EXIT_BAD_SIGNATURE


def _do_make_credential(args, out: dict):
    result = yubikey.make_credential(
        rp_id=args.rp_id,
        attestation=args.attestation,
        user_verification=args.user_verification,
        user_interaction=_user_interaction(),
    )
    out["credential"] = credential_report(result)
    _print(out["credential"], as_json=args.json, title="credential")

    if args.out:
        yubikey.save_result(result, args.out)
        out["saved_to"] = str(args.out)
        if not args.json:
            print(f"\nsaved to {args.out} - derive later with: "
                  f"py tools/yubikey_exec.py derive --in {args.out}")
    return result


def _do_derive(args, result, out: dict):
    derived = yubikey.seed_public_key(
        result, ctx=args.ctx.encode("utf-8"), ikm=parse_ikm(args.ikm)
    )
    out["derived"] = derived_report(derived)
    _print(out["derived"], as_json=args.json, title="derived key")
    return derived


# --- CLI -----------------------------------------------------------------------
def _add_attest_flags(parser: argparse.ArgumentParser) -> None:
    """The YubiKey check is opt-in: nothing here changes behaviour by default."""
    group = parser.add_argument_group("optional YubiKey attestation check")
    group.add_argument(
        "--attest", action="store_true", help="run the 'is this a YubiKey?' check and report it"
    )
    group.add_argument(
        "--require-yubikey",
        action="store_true",
        help=f"imply --attest and exit {EXIT_NOT_YUBIKEY} unless it verifies as a YubiKey",
    )
    group.add_argument(
        "--roots", metavar="PEM", help="trust anchors to pin against (default: bundled Yubico roots)"
    )
    group.add_argument(
        "--no-require-root",
        action="store_true",
        help="skip chain pinning; only verify the statement and the Yubico name",
    )
    group.add_argument(
        "--seed-attestation",
        action="store_true",
        help="check the seed key's own attestation instead of the credential's",
    )


def _add_derive_flags(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--ctx", default=DEFAULT_CTX, help="derivation context label (default: %(default)s)")
    parser.add_argument("--ikm", help="hex input keying material (default: 32 random bytes)")


def _add_payload_flags(parser: argparse.ArgumentParser, *, required: bool = True) -> None:
    """What gets signed: a string, or a ready-made SHA-256 digest."""
    what = parser.add_mutually_exclusive_group(required=required)
    what.add_argument("--message", help="string to sign (UTF-8; hashed with SHA-256)")
    what.add_argument("--digest", help="hex SHA-256 digest to sign, when the data itself is not at hand")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="yubikey-exec", description="Inspect a YubiKey and drive the ARKG (previewSign) flow."
    )
    parser.add_argument("--json", action="store_true", help="emit one JSON document on stdout")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("version", help="firmware version / AAGUID / previewSign support (no touch)")

    p_mc = sub.add_parser("make-credential", help="makeCredential(previewSign) - needs a touch")
    p_mc.add_argument("--out", type=Path, help="save the result for a later `derive`")
    p_mc.add_argument("--rp-id", default="example.com")
    p_mc.add_argument("--attestation", choices=("direct", "none"), default="direct")
    p_mc.add_argument("--user-verification", choices=("discouraged", "preferred", "required"), default="discouraged")
    _add_attest_flags(p_mc)

    p_dv = sub.add_parser("derive", help="derive_public_key from a saved credential (no device)")
    p_dv.add_argument("--in", dest="input", type=Path, required=True, help="file written by make-credential --out")
    _add_derive_flags(p_dv)
    _add_attest_flags(p_dv)

    p_sg = sub.add_parser("sign", help="sign data with a derived key, then verify it (needs a touch)")
    p_sg.add_argument("--in", dest="input", type=Path, required=True, help="file written by make-credential --out")
    _add_derive_flags(p_sg)
    _add_payload_flags(p_sg)
    p_sg.add_argument("--rp-id", default="example.com")
    p_sg.add_argument("--user-verification", choices=("discouraged", "preferred", "required"), default="discouraged")

    p_vf = sub.add_parser("verify", help="verify a signature offline (re-derives the key)")
    p_vf.add_argument("--in", dest="input", type=Path, required=True, help="file written by make-credential --out")
    p_vf.add_argument("--signature", required=True, help="hex signature to check")
    _add_derive_flags(p_vf)
    _add_payload_flags(p_vf)

    p_run = sub.add_parser("run", help="make-credential + derive in one go")
    p_run.add_argument("--out", type=Path, help="also save the credential")
    p_run.add_argument("--rp-id", default="example.com")
    p_run.add_argument("--attestation", choices=("direct", "none"), default="direct")
    p_run.add_argument("--user-verification", choices=("discouraged", "preferred", "required"), default="discouraged")
    _add_derive_flags(p_run)
    # Optional here: `run` is useful with or without a signing step.
    _add_payload_flags(p_run, required=False)
    _add_attest_flags(p_run)

    return parser


_COMMANDS = {
    "version": cmd_version,
    "make-credential": cmd_make_credential,
    "derive": cmd_derive,
    "sign": cmd_sign,
    "verify": cmd_verify,
    "run": cmd_run,
}


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        return _COMMANDS[args.cmd](args)
    # ValueError/TypeError are the documented contract of the library's validators
    # (validate_ikm / validate_ctx) — for a CLI those are bad user input, not bugs,
    # so they get the same "message + exit 1" treatment as YubiKeyError.
    except (yubikey.YubiKeyError, ValueError, TypeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT_ERROR
    except KeyboardInterrupt:
        print("aborted", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    raise SystemExit(main())
