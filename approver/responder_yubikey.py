"""responder_yubikey.py - approval responder whose signing key lives on a YubiKey.

Same role and same wire protocol as ``responder.py`` (CLAUDE.md §6/§7), but there
is no private key on this machine: the key pair is an ARKG key derived from the
authenticator's seed key (§8), so the private half exists only inside the YubiKey
and every decision costs a physical touch.

  register <token>   Get an ARKG seed key from the YubiKey (a touch - or reuse a
                     credential saved earlier by `yubikey-exec make-credential`,
                     which needs no device), derive a fresh key pair from it,
                     register the derived *public* key over ``registrations`` with
                     a one-time token, and store what is needed to re-derive it in
                     responder-yubikey-config.json. Written only on ``ok:true``.

  serve              Subscribe to ``approvals.*``, prompt the operator (a single
                     a/d/s keystroke - no reason is asked for, the touch is enough
                     ceremony), then have the YubiKey sign the decision (second
                     touch, once per decision) and reply. Each signed decision is
                     echoed back on the console with a fingerprint of its signature.

Why the hook needs no changes: an ARKG derived key is a P-256 key and the
authenticator signs ECDSA-P256 over SHA-256, DER-encoded - exactly
``lib.crypto``'s ``key_type="p256"`` scheme. Registration publishes the derived key
as the compressed SEC1 point that scheme expects (``yubikey.p256_public_b64``), so
``hook.py`` verifies a YubiKey reply with the same code path as a software one.

What the config stores: the credential (public data only), the derivation label
``ctx`` and the ``ikm`` - together they re-derive the *public* key and let the
device reconstruct the private half on demand. Losing the file means re-registering;
leaking it does not leak a signing key. Keep it private all the same: it identifies
the credential and links the derived key to it.

Windows: the CTAP2 ``previewSign`` extension needs an **administrator** terminal,
and elevation drops the virtualenv - so run the venv interpreter by path (§8):
  sudo E:\\projects\\ai-remote\\.venv\\Scripts\\python.exe approver\\responder_yubikey.py serve
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import binascii
import hashlib
import sys
import time
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Any

# Non-package project: make repo-root imports (`lib`, `approver`) work when this
# file is run directly as a script (script dir alone is not enough).
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from approver import protocol, responder  # noqa: E402
from lib import bus  # noqa: E402
from lib import config as configlib  # noqa: E402
from lib import crypto  # noqa: E402
from lib import yubikey  # noqa: E402

DEFAULT_CONFIG = Path(__file__).resolve().parent / "responder-yubikey-config.json"
#: Purpose label the derived key is scoped to (§8: ARKG ``ctx``).
DEFAULT_CTX = "ai-remote-approvals"
#: The credential's relying party. Must match the one it was created with, so it is
#: persisted at registration and read back by ``serve``.
DEFAULT_RP_ID = "example.com"
DEFAULT_USER_VERIFICATION = "discouraged"

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_NOT_YUBIKEY = 2

_TOUCH_MESSAGE = "\n>>> touch your YubiKey to sign this decision <<<\n"
_REQUIRED_CONFIG_FIELDS = ("key_id", "public_key", "ctx", "ikm", "credential")


class NotAYubiKey(Exception):
    """The attestation check said this is not a genuine YubiKey (and it was required).

    Carries the :class:`lib.yubikey.AttestationCheck` so the caller can report the
    precise reasons instead of a bare "rejected".
    """

    def __init__(self, check: yubikey.AttestationCheck):
        super().__init__("credential did not verify as a YubiKey: " + "; ".join(check.reasons))
        self.check = check


# --- config: what has to survive between register and serve --------------------
def config_from_derivation(
    key_id: str,
    result: yubikey.MakeCredentialResult,
    derived: yubikey.DerivedKey,
    *,
    rp_id: str = DEFAULT_RP_ID,
) -> dict:
    """The responder-yubikey config for a freshly derived key.

    Deliberately holds no private key: ``credential`` + ``ctx`` + ``ikm`` re-derive
    the public half offline, and the YubiKey reconstructs the private half from the
    key handle when it signs.
    """
    return {
        "v": protocol.PROTOCOL_VERSION,
        "key_id": key_id,
        "key_type": crypto.P256,
        "public_key": yubikey.p256_public_b64(derived),
        "ctx": derived.ctx.decode("utf-8"),
        "ikm": derived.ikm.hex(),
        "rp_id": rp_id,
        "credential": yubikey.result_to_dict(result),
    }


def derived_from_config(
    data: Mapping[str, Any],
) -> tuple[yubikey.MakeCredentialResult, yubikey.DerivedKey]:
    """Rebuild the credential and re-derive the registered key. No device, no touch.

    Also re-checks that the derivation still yields the ``public_key`` that was
    registered. A mismatch (edited ``ikm``/``ctx``, swapped credential) means every
    signature this responder produced would be rejected by the hook, which looks
    like a mysterious fall-back to the interactive prompt - so fail loudly at
    startup instead.
    """
    for field in _REQUIRED_CONFIG_FIELDS:
        if not data.get(field):
            raise configlib.ConfigError(
                f"responder-yubikey config is missing {field!r} - run: "
                f"responder_yubikey.py register <token>"
            )

    result = yubikey.result_from_dict(data["credential"])
    try:
        ikm = bytes.fromhex(data["ikm"])
        yubikey.validate_ikm(ikm)
    except (TypeError, ValueError) as e:
        raise configlib.ConfigError(f"config 'ikm' is unusable: {e}") from e

    derived = yubikey.seed_public_key(result, ctx=str(data["ctx"]).encode("utf-8"), ikm=ikm)
    if yubikey.p256_public_b64(derived) != data["public_key"]:
        raise configlib.ConfigError(
            "the config's credential/ctx/ikm no longer derive the registered public key "
            "- signatures made now would be rejected by the hook. Re-register with a "
            "fresh token."
        )
    return result, derived


# --- signing on the device -----------------------------------------------------
def device_signer(
    result: yubikey.MakeCredentialResult,
    derived: yubikey.DerivedKey,
    *,
    rp_id: str = DEFAULT_RP_ID,
    user_verification: str = DEFAULT_USER_VERIFICATION,
    user_interaction: Any = None,
    self_verify: bool = True,
) -> Callable[[bytes], str]:
    """A signer for :func:`approver.responder.build_signed_reply`, backed by the key.

    The returned callable takes the §7 signing bytes and returns the base64 DER
    signature. It blocks on a touch, so it must be called off the event loop (which
    ``make_approval_handler`` arranges).

    ``self_verify`` re-checks the signature against the derived public key before
    handing it over: a reply the registered key cannot verify would be rejected by
    the hook anyway, and failing here surfaces the reason to the operator instead.
    """

    def sign(message: bytes) -> str:
        signature = yubikey.sign_with_derived_key(
            result,
            derived,
            data=message,
            rp_id=rp_id,
            user_verification=user_verification,
            user_interaction=user_interaction,
        )
        if self_verify and not yubikey.verify_signature(derived, signature, data=message):
            raise yubikey.YubiKeyError(
                "the YubiKey's signature does not verify against the registered public "
                "key - not sending it"
            )
        return base64.b64encode(signature).decode("ascii")

    return sign


# --- commands ------------------------------------------------------------------
def _report_attestation(check: yubikey.AttestationCheck) -> None:
    print(f"  is_yubikey    : {check.is_yubikey}", file=sys.stderr)
    print(f"  certificate   : {check.certificate_subject}", file=sys.stderr)
    print(f"  trusted root  : {check.trusted_root_subject}", file=sys.stderr)
    print(f"  aaguid        : {check.aaguid_hex}", file=sys.stderr)
    for reason in check.reasons:
        print(f"  ! {reason}", file=sys.stderr)


async def register(
    token: str,
    *,
    credential_path: Path | str | None = None,
    ctx: str = DEFAULT_CTX,
    ikm: bytes | None = None,
    config_path: Path | str = DEFAULT_CONFIG,
    servers: str = bus.DEFAULT_SERVERS,
    timeout: float = 10.0,
    rp_id: str = DEFAULT_RP_ID,
    attest: bool = False,
    require_yubikey: bool = False,
    roots: Sequence[bytes] | None = None,
    intermediates: Sequence[bytes] | None = None,
    require_root: bool = True,
    user_interaction: Any = None,
) -> dict:
    """Register/rotate a YubiKey-backed key. Persists only on ``ok:true``.

    Without ``credential_path`` this runs ``makeCredential`` on the connected
    YubiKey (one touch). With it, an already-saved credential is reused and no
    device is needed at all - derivation is offline. ``ikm`` defaults to 32 fresh
    random bytes, so re-registering the same YubiKey yields an unlinkable new key.

    Set ``require_yubikey`` to abort unless the credential's attestation pins to a
    Yubico root; nothing is published or persisted in that case. Returns the
    handler's reply dict.

    Raises ``ValueError`` on a malformed token, :class:`NotAYubiKey` on a rejected
    attestation, ``lib.yubikey.YubiKeyError`` on device/credential problems and
    ``bus.BusError`` on transport failure.
    """
    key_id = responder.parse_key_id(token)

    if credential_path is not None:
        result = yubikey.load_result(credential_path)
    else:
        print("makeCredential on the YubiKey (this needs a touch)", file=sys.stderr)
        result = yubikey.make_credential(
            rp_id=rp_id,
            user_interaction=user_interaction or yubikey.console_user_interaction(),
        )

    if attest or require_yubikey:
        check = yubikey.verify_yubikey_attestation(
            result, roots=roots, intermediates=intermediates, require_root=require_root
        )
        print("attestation:", file=sys.stderr)
        _report_attestation(check)
        if require_yubikey and not check.is_yubikey:
            raise NotAYubiKey(check)

    derived = yubikey.seed_public_key(result, ctx=ctx.encode("utf-8"), ikm=ikm)
    request = responder.build_registration_request(
        token,
        yubikey.p256_public_b64(derived),
        int(time.time()),
        key_type=crypto.P256,
    )

    async with bus.connect(servers) as b:
        reply = await b.request("registrations", request, timeout=timeout)

    if reply.get("ok"):
        configlib.Config(
            config_path, config_from_derivation(key_id, result, derived, rp_id=rp_id)
        ).save()
    return reply


def prompt_operator(request: dict):
    """One keystroke, no reason asked. Returns ``(behavior, "", None)`` or None to skip.

    Same display as :func:`approver.responder.prompt_operator`, but the free-text
    ``reason`` prompt is dropped: every decision here already costs a physical touch,
    and a second question between the keystroke and the touch only slows the operator
    down. ``reason`` still travels (empty) and is still covered by the signature (§7).
    """
    responder.print_request(request)
    answer = input("allow / deny / skip? [a/d/s]: ").strip().lower()
    if answer in ("a", "allow"):
        return ("allow", "", None)
    if answer in ("d", "deny"):
        return ("deny", "", None)
    # ASCII only: a Windows console with a legacy codepage mangles anything else.
    print("skipped (no reply - hook falls back to the interactive prompt)", file=sys.stderr)
    return None


def signature_fingerprint(sig_b64: str) -> str:
    """sha256 of the signature bytes, hex - what the console shows after a decision.

    The whole digest: the raw DER signature is longer still and means nothing by eye,
    while a truncated hash is not something an operator can compare against anything.
    """
    return hashlib.sha256(base64.b64decode(sig_b64, validate=True)).hexdigest()


def print_decision(reply: Mapping[str, Any]) -> None:
    """Confirm on the console what was signed and sent (§8.7).

    Neither the keystroke nor the touch echoes the outcome, so between "I pressed a"
    and "the key blinked" there is nothing telling the operator what actually left
    the machine. Prints the decision plus a fingerprint of the signature.

    Never raises: it runs after a decision is signed and about to be published.
    """
    print(f"  decision  : {reply.get('behavior')}", file=sys.stderr)
    try:
        digest = "sha256:" + signature_fingerprint(str(reply.get("sig", "")))
    except (binascii.Error, TypeError, ValueError) as e:
        digest = f"<unreadable signature: {e}>"
    print(f"  signature : {digest}", file=sys.stderr)
    print("  sent to the hook\n", file=sys.stderr)


async def serve(
    *,
    config_path: Path | str = DEFAULT_CONFIG,
    servers: str = bus.DEFAULT_SERVERS,
    subject: str = responder.DEFAULT_SUBJECT,
    queue: str = responder.DEFAULT_QUEUE,
    prompt=prompt_operator,
    on_signed=print_decision,
    user_verification: str = DEFAULT_USER_VERIFICATION,
    user_interaction: Any = None,
) -> None:
    """Answer approval requests until interrupted; each decision needs a touch."""
    try:
        cfg = configlib.Config.load(config_path)
    except configlib.ConfigError as e:
        raise configlib.ConfigError(
            f"{e} - run: responder_yubikey.py register <token>"
        ) from e

    # Re-derivation is offline: the YubiKey is not needed until the first signature.
    result, derived = derived_from_config(cfg.data)
    key_id = cfg["key_id"]

    handler = responder.make_approval_handler(
        key_id=key_id,
        sign=device_signer(
            result,
            derived,
            rp_id=cfg.get("rp_id", DEFAULT_RP_ID),
            user_verification=user_verification,
            user_interaction=user_interaction
            or yubikey.console_user_interaction(touch_message=_TOUCH_MESSAGE),
        ),
        prompt=prompt,
        on_signed=on_signed,
    )

    async with bus.connect(servers) as b:
        await b.reply(subject, handler, queue=queue)
        print(
            f"yubikey responder key_id={key_id!r} (p256, ctx={cfg['ctx']!r}) serving "
            f"{subject!r} (queue={queue!r}) - keep the key plugged in, every decision "
            f"needs a touch. Ctrl+C to stop",
            file=sys.stderr,
        )
        await asyncio.Event().wait()  # run until cancelled / interrupted


# --- CLI -----------------------------------------------------------------------
def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="responder_yubikey.py",
        description="Approval responder whose signing key lives on a YubiKey (ARKG).",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_reg = sub.add_parser("register", help="register/rotate a YubiKey-backed key")
    p_reg.add_argument("token", help="one-time token '<key_id>.<secret>' from the handler")
    p_reg.add_argument(
        "--credential",
        metavar="FILE",
        help="reuse a credential saved by `yubikey-exec make-credential --out` "
        "(no device needed); pass the --rp-id it was created with",
    )
    p_reg.add_argument(
        "--ctx", default=DEFAULT_CTX, help="derivation context label (default: %(default)s)"
    )
    p_reg.add_argument("--ikm", help="hex input keying material (default: 32 random bytes)")
    p_reg.add_argument("--rp-id", default=DEFAULT_RP_ID)
    p_reg.add_argument("--config", default=str(DEFAULT_CONFIG))
    p_reg.add_argument("--servers", default=bus.DEFAULT_SERVERS)
    p_reg.add_argument("--timeout", type=float, default=10.0)

    group = p_reg.add_argument_group("optional YubiKey attestation check")
    group.add_argument(
        "--attest", action="store_true", help="run the 'is this a YubiKey?' check and report it"
    )
    group.add_argument(
        "--require-yubikey",
        action="store_true",
        help=f"imply --attest and refuse to register (exit {EXIT_NOT_YUBIKEY}) unless it verifies",
    )
    group.add_argument("--roots", metavar="PEM", help="trust anchors (default: bundled Yubico roots)")
    group.add_argument(
        "--intermediates", metavar="PEM", help="bridging CAs (default: bundled Yubico ones)"
    )
    group.add_argument(
        "--no-require-root", action="store_true", help="skip chain pinning (dev keys)"
    )

    p_srv = sub.add_parser("serve", help="answer approval requests, signing on the key")
    p_srv.add_argument("--config", default=str(DEFAULT_CONFIG))
    p_srv.add_argument("--servers", default=bus.DEFAULT_SERVERS)
    p_srv.add_argument("--subject", default=responder.DEFAULT_SUBJECT)
    p_srv.add_argument("--queue", default=responder.DEFAULT_QUEUE)
    p_srv.add_argument(
        "--user-verification",
        choices=("discouraged", "preferred", "required"),
        default=DEFAULT_USER_VERIFICATION,
    )
    return parser


def _cmd_register(args) -> int:
    reply = asyncio.run(
        register(
            args.token,
            credential_path=args.credential,
            ctx=args.ctx,
            ikm=bytes.fromhex(args.ikm) if args.ikm else None,
            config_path=Path(args.config),
            servers=args.servers,
            timeout=args.timeout,
            rp_id=args.rp_id,
            attest=args.attest,
            require_yubikey=args.require_yubikey,
            roots=yubikey.load_yubico_roots(args.roots) if args.roots else None,
            intermediates=(
                yubikey.load_yubico_intermediates(args.intermediates)
                if args.intermediates
                else None
            ),
            require_root=not args.no_require_root,
        )
    )
    if reply.get("ok"):
        print(f"registered key_id={reply.get('key_id')} (p256, ctx={args.ctx})")
        print(f"config: {args.config} - it holds no private key; the YubiKey does")
        return EXIT_OK
    print(f"registration rejected: {reply.get('error')}", file=sys.stderr)
    return EXIT_ERROR


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)
    # ValueError/TypeError are the documented contract of the token parser and the
    # library validators - for a CLI those are bad input, not bugs.
    errors = (
        yubikey.YubiKeyError,
        configlib.ConfigError,
        bus.BusError,
        ValueError,
        TypeError,
    )

    if args.cmd == "register":
        try:
            return _cmd_register(args)
        except NotAYubiKey as e:
            print("not verified as a YubiKey - nothing registered:", file=sys.stderr)
            for reason in e.check.reasons:
                print(f"  ! {reason}", file=sys.stderr)
            return EXIT_NOT_YUBIKEY
        except errors as e:
            print(f"registration failed: {e}", file=sys.stderr)
            return EXIT_ERROR
        except KeyboardInterrupt:
            print("aborted", file=sys.stderr)
            return EXIT_ERROR

    if args.cmd == "serve":
        try:
            asyncio.run(
                serve(
                    config_path=Path(args.config),
                    servers=args.servers,
                    subject=args.subject,
                    queue=args.queue,
                    user_verification=args.user_verification,
                )
            )
        except KeyboardInterrupt:
            return EXIT_OK
        except errors as e:
            print(f"serve failed: {e}", file=sys.stderr)
            return EXIT_ERROR
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
