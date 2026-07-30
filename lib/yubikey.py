"""YubiKey / ARKG helpers over the optional ``fido2`` extra (CLAUDE.md §8).

Four things, in the order you normally need them:

1. :func:`get_device_info` / :func:`get_version` — which YubiKey is plugged in,
   what firmware it runs, and whether it can do ARKG at all.
2. :func:`make_credential` — a WebAuthn ``makeCredential`` with the ``previewSign``
   extension asking for an ARKG **seed** key ("master" key) on the device.
3. :func:`seed_public_key` — offline, from that result: derive a fresh
   ``derived_public_key`` plus the ``arkg_args`` (COSE_Sign_Args) you hand back to
   the authenticator when you later ask it to sign. No device, no touch.
4. :func:`verify_yubikey_attestation` — is this actually a YubiKey? Verifies the
   attestation statement and pins the certificate chain to a Yubico root.

``fido2`` is an **optional** dependency (``uv sync --extra yubikey``). This module
imports fine without it: the pure helpers (version decoding, ikm/ctx validation,
trust-anchor loading) work anywhere, and everything that actually needs ``fido2``
raises :class:`Fido2NotInstalled` with an actionable hint.

ARKG in one paragraph: the authenticator keeps a seed key pair. Anyone holding the
seed *public* key can derive unlimited fresh public keys offline — unlinkable to
each other and to the seed — by picking a random ``ikm`` and a purpose label
``ctx``. Only the authenticator can produce the matching private key, and only
when handed the ``key_handle`` from the derivation. That is why step 3 needs no
hardware, and why ``ikm`` must be unpredictable.

Reference: Yubico's ``example_arkg.py`` from YubicoLabs/build-with-us. Note that
the published example targets an older ``previewSign`` snapshot where the generated
key arrived as a websafe-base64 dict; ``fido2`` 2.2.1 returns a
``_SignGeneratedKey`` dataclass (raw ``bytes`` + a parsed ``AttestationObject``),
which is what this module speaks.

Hardware requirement: a YubiKey on firmware 5.8.0+ advertising ``previewSign``.
On Windows, run from an **administrator** terminal — otherwise the native WebAuthn
path is used and the extension output never comes back (see :func:`make_credential`).

This module is a library only — the command-line front end is
``tools/yubikey_exec.py`` (the ``yubikey-exec`` utility).
"""
from __future__ import annotations

import base64
import json
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Iterator, Mapping, Sequence

from cryptography import x509
from cryptography.hazmat.primitives.serialization import Encoding

# --- optional dependency -------------------------------------------------------
# Imported eagerly so callers can branch on FIDO2_AVAILABLE, but never fatally:
# the pure helpers below must keep working on a bare `uv sync`.
try:  # pragma: no cover - trivially environment-dependent
    from fido2 import cbor as _cbor
    from fido2.cose import (
        ARKG_P256_PLACEHOLDER as _ARKG_P256,
        ESP256_SPLIT_ARKG_PLACEHOLDER as _ARKG_SPLIT_ALG,
        CoseKey as _CoseKey,
    )

    FIDO2_AVAILABLE = True
    _IMPORT_ERROR: Exception | None = None
except ImportError as e:  # pragma: no cover - exercised by monkeypatching instead
    _cbor = None  # type: ignore[assignment]
    _ARKG_P256 = None  # type: ignore[assignment]
    _ARKG_SPLIT_ALG = None  # type: ignore[assignment]
    _CoseKey = None  # type: ignore[assignment]
    FIDO2_AVAILABLE = False
    _IMPORT_ERROR = e

#: The CTAP2 extension that carries ARKG key generation. Kept as a plain string so
#: this module can answer "does this device support it?" without importing fido2.
PREVIEW_SIGN_NAME = "previewSign"

#: previewSign / ARKG landed in YubiKey firmware 5.8.0.
_MIN_ARKG_RAW = 0x050800

#: The ARKG draft recommends at least 256 bits of entropy for ``ikm``.
MIN_IKM_BYTES = 32

#: Yubico FIDO/U2F attestation roots, shipped next to this module.
YUBICO_ROOTS_PEM = Path(__file__).resolve().with_name("yubico-fido-ca.pem")

#: Yubico FIDO attestation *intermediates*, shipped next to this module. Not trust
#: anchors - a YubiKey ships only its end-entity certificate in x5c, so these are
#: needed to bridge it to a root in :data:`YUBICO_ROOTS_PEM`.
YUBICO_INTERMEDIATES_PEM = Path(__file__).resolve().with_name("yubico-fido-intermediates.pem")

_EXTRA_HINT = "uv sync --extra yubikey"


# --- errors --------------------------------------------------------------------
class YubiKeyError(Exception):
    """Base class for every failure in this module."""


class Fido2NotInstalled(YubiKeyError):
    """The optional ``fido2`` extra is not installed."""


class NoAuthenticator(YubiKeyError):
    """No (suitable) authenticator was found."""


class ExtensionUnsupported(YubiKeyError):
    """The device or the credential cannot do what was asked (e.g. no ARKG)."""


class AttestationError(YubiKeyError):
    """An attestation object could not be checked, or is not a YubiKey's."""


def _is_admin() -> bool:
    """True if this process is elevated (Windows only; False elsewhere/on error)."""
    if os.name != "nt":
        return False
    try:
        import ctypes

        return bool(ctypes.windll.shell32.IsUserAnAdmin())  # type: ignore[attr-defined]
    except Exception:  # noqa: BLE001 - diagnostics must never raise
        return False


def no_authenticator_hint(*, is_windows: bool, is_admin: bool) -> str:
    """Message for "no device found", naming the usual Windows cause when it applies.

    Windows routes FIDO authenticators through the WebAuthn API and denies raw HID
    access to unelevated processes, so ``CtapHidDevice.list_devices()`` comes back
    empty even with a key plugged in. Reporting only "is it plugged in?" sends people
    hunting for a hardware fault that isn't there.
    """
    if is_windows and not is_admin:
        return (
            "no FIDO authenticator found. This process is not elevated, and Windows "
            "only exposes FIDO devices to the WebAuthn API for unelevated processes - "
            "so the device list is empty even with a YubiKey plugged in. Re-run from an "
            "administrator terminal (also required for previewSign output)."
        )
    return "no FIDO authenticator found (is the YubiKey plugged in?)"


def no_authenticator_error() -> "NoAuthenticator":
    """Build a :class:`NoAuthenticator` carrying the platform-aware hint."""
    return NoAuthenticator(
        no_authenticator_hint(is_windows=os.name == "nt", is_admin=_is_admin())
    )


def require_fido2() -> None:
    """Raise :class:`Fido2NotInstalled` unless the optional extra is importable."""
    if not FIDO2_AVAILABLE:
        raise Fido2NotInstalled(
            f"this feature needs the optional 'fido2' dependency: {_EXTRA_HINT} "
            f"(import failed: {_IMPORT_ERROR})"
        )


# --- firmware version (pure) ---------------------------------------------------
@dataclass(frozen=True, order=True)
class FirmwareVersion:
    """A YubiKey firmware version, ordered by major/minor/patch.

    ``raw`` keeps the packed integer CTAP2 reported and is excluded from
    comparisons, so two objects decoded from the same value compare equal.
    """

    major: int
    minor: int
    patch: int
    raw: int = field(default=0, compare=False)

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"

    def as_tuple(self) -> tuple[int, int, int]:
        return (self.major, self.minor, self.patch)

    @property
    def is_known(self) -> bool:
        """False when the device did not report a version (CTAP2 defaults it to 0)."""
        return self.raw > 0


def parse_firmware_version(raw: int) -> FirmwareVersion:
    """Decode CTAP2's packed ``firmwareVersion`` (``major<<16 | minor<<8 | patch``).

    YubiKeys report e.g. ``0x050800`` (328704) for 5.8.0. ``0`` means the field was
    absent — the result's :attr:`FirmwareVersion.is_known` is then False.
    """
    # bool is an int subclass; a stray True is not a version.
    if isinstance(raw, bool) or not isinstance(raw, int):
        raise TypeError(f"firmware version must be an int, got {type(raw).__name__}")
    if raw < 0:
        raise ValueError(f"firmware version must not be negative, got {raw}")
    if raw > 0xFFFFFF:
        raise ValueError(f"firmware version does not fit in three bytes: {raw:#x}")
    return FirmwareVersion((raw >> 16) & 0xFF, (raw >> 8) & 0xFF, raw & 0xFF, raw)


#: Minimum firmware that can do previewSign / ARKG.
MIN_ARKG_FIRMWARE = parse_firmware_version(_MIN_ARKG_RAW)


# --- device info (pure mapping over a CTAP2 Info) ------------------------------
@dataclass(frozen=True)
class DeviceInfo:
    """What ``getInfo`` says about the connected authenticator."""

    firmware_version: FirmwareVersion
    aaguid: bytes
    extensions: tuple[str, ...]
    versions: tuple[str, ...]
    supports_preview_sign: bool

    @property
    def aaguid_hex(self) -> str:
        return self.aaguid.hex()

    @property
    def meets_arkg_firmware(self) -> bool:
        """True when the reported firmware is >= 5.8.0 (ARKG's floor)."""
        return self.firmware_version.is_known and self.firmware_version >= MIN_ARKG_FIRMWARE


def supports_preview_sign(info: Any) -> bool:
    """True iff ``info`` advertises the ``previewSign`` extension.

    Accepts any object with an ``extensions`` sequence, and ``None`` — the Windows
    WebAuthn client path yields no CTAP2 info at all.
    """
    if info is None:
        return False
    extensions = getattr(info, "extensions", None) or ()
    return PREVIEW_SIGN_NAME in extensions


def device_info_from_info(info: Any) -> DeviceInfo:
    """Map a CTAP2 ``Info`` (or any duck-typed stand-in) onto :class:`DeviceInfo`."""
    return DeviceInfo(
        firmware_version=parse_firmware_version(getattr(info, "firmware_version", 0) or 0),
        aaguid=bytes(getattr(info, "aaguid", b"") or b""),
        extensions=tuple(getattr(info, "extensions", None) or ()),
        versions=tuple(getattr(info, "versions", None) or ()),
        supports_preview_sign=supports_preview_sign(info),
    )


# --- ikm / ctx validation (pure) -----------------------------------------------
def validate_ikm(ikm: bytes) -> None:
    """Reject input keying material that is too short or not bytes.

    ``ikm`` is what makes each derived key unique and unlinkable; a predictable or
    short value undermines the whole point, so this is a hard error, not a warning.
    """
    if not isinstance(ikm, (bytes, bytearray, memoryview)):
        raise TypeError(f"ikm must be bytes, got {type(ikm).__name__}")
    if len(ikm) < MIN_IKM_BYTES:
        raise ValueError(
            f"ikm must be at least {MIN_IKM_BYTES} bytes ({MIN_IKM_BYTES * 8} bits of "
            f"entropy, per the ARKG draft), got {len(ikm)}"
        )


def validate_ctx(ctx: bytes) -> None:
    """Reject an empty or non-bytes derivation context label."""
    if not isinstance(ctx, (bytes, bytearray, memoryview)):
        raise TypeError(f"ctx must be bytes, got {type(ctx).__name__}")
    if not len(ctx):
        raise ValueError("ctx must be a non-empty label scoping the key to a purpose")


# --- results -------------------------------------------------------------------
@dataclass(frozen=True)
class MakeCredentialResult:
    """Outcome of :func:`make_credential` — everything later steps need.

    ``seed_public_key_cbor`` is the CBOR COSE encoding of the ARKG **seed** public
    key; :func:`seed_public_key` turns it into derived keys. ``attestation_object``
    is the credential's own attestation (signed over ``authData || clientDataHash``)
    and is what :func:`verify_yubikey_attestation` checks by default;
    ``seed_attestation_object`` is the separate attestation the ``previewSign``
    extension returns for the generated seed key.
    """

    key_handle: bytes
    seed_public_key_cbor: bytes
    algorithm: int
    credential_id: bytes
    aaguid: bytes
    client_data_hash: bytes
    attestation_object: Any = None
    registration_response: Any = None
    seed_attestation_object: Any = None

    @property
    def aaguid_hex(self) -> str:
        return self.aaguid.hex()


@dataclass(frozen=True)
class DerivedKey:
    """A freshly derived ARKG public key plus the args the authenticator will need.

    Keep ``arkg_args`` (or ``arkg_args_cbor``) and ``key_handle`` together with the
    ``derived_public_key``: signing later requires handing both back to the device,
    and verifying the signature requires the derived public key.
    """

    derived_public_key: Any
    derived_public_key_cbor: bytes
    arkg_args: Mapping
    arkg_args_cbor: bytes
    ikm: bytes
    ctx: bytes
    key_handle: bytes
    seed_public_key: Any


@dataclass(frozen=True)
class AttestationCheck:
    """Verdict of :func:`verify_yubikey_attestation`.

    ``is_yubikey`` is the single answer; ``reasons`` explains a False (and is empty
    on success), so a caller can log precisely what failed.
    """

    is_yubikey: bool
    attestation_verified: bool
    reasons: tuple[str, ...]
    aaguid: bytes
    fmt: str
    attestation_type: str | None
    certificate_subject: str
    certificate_issuer: str
    trusted_root_subject: str | None
    chain_length: int

    @property
    def aaguid_hex(self) -> str:
        return self.aaguid.hex()


# --- step 3: seed public key -> derived public key + arkg args ------------------
def seed_public_key(
    result: MakeCredentialResult, *, ctx: bytes, ikm: bytes | None = None
) -> DerivedKey:
    """Derive a fresh public key from the ARKG seed key in ``result``. Offline.

    ``ctx`` is a label scoping the key to a purpose; ``ikm`` is the randomness that
    makes the derived key unique and unlinkable (generated as 32 random bytes when
    omitted). Same ``(ikm, ctx)`` reproduces the same key — different values give a
    different, unlinkable one.

    Returns the ``derived_public_key`` to verify signatures with, and ``arkg_args``
    (COSE_Sign_Args) to pass to the authenticator alongside ``key_handle`` when
    asking it to sign.
    """
    require_fido2()
    validate_ctx(ctx)
    if ikm is None:
        ikm = os.urandom(MIN_IKM_BYTES)
    validate_ikm(ikm)

    seed = parse_seed_public_key(result.seed_public_key_cbor)
    derived, args = seed.derive_public_key(bytes(ikm), bytes(ctx))

    return DerivedKey(
        derived_public_key=derived,
        derived_public_key_cbor=_cbor.encode(dict(derived)),
        arkg_args=args,
        arkg_args_cbor=_cbor.encode(dict(args)),
        ikm=bytes(ikm),
        ctx=bytes(ctx),
        key_handle=result.key_handle,
        seed_public_key=seed,
    )


def parse_seed_public_key(seed_cbor: bytes) -> Any:
    """Parse a CBOR COSE blob into an ARKG seed key, or raise.

    Split out from :func:`seed_public_key` so a caller can validate a stored seed
    key without deriving anything.
    """
    require_fido2()
    try:
        parsed = _CoseKey.parse(_cbor.decode(seed_cbor))
    except Exception as e:  # noqa: BLE001 — any decode/parse problem is one error to us
        raise YubiKeyError(f"seed public key is not a readable COSE key: {e}") from e
    if not isinstance(parsed, _ARKG_P256):
        raise ExtensionUnsupported(
            f"seed public key is {type(parsed).__name__} (alg {parsed.get(3)}), not an "
            f"ARKG-P256 key - the credential was not created with previewSign/ARKG"
        )
    return parsed


# --- step 4: attestation -------------------------------------------------------
def _load_pem_bundle(pem: Path, *, what: str) -> list[bytes]:
    try:
        certs = x509.load_pem_x509_certificates(pem.read_bytes())
    except (OSError, ValueError) as e:
        raise AttestationError(f"cannot load {what} from {pem}: {e}") from e
    return [c.public_bytes(Encoding.DER) for c in certs]


def load_yubico_roots(path: str | os.PathLike[str] | None = None) -> list[bytes]:
    """Load the bundled Yubico FIDO attestation roots as DER blobs.

    Needs only ``cryptography``, so it works without the ``fido2`` extra.
    """
    pem = Path(path) if path is not None else YUBICO_ROOTS_PEM
    return _load_pem_bundle(pem, what="Yubico trust anchors")


def load_yubico_intermediates(path: str | os.PathLike[str] | None = None) -> list[bytes]:
    """Load the bundled Yubico FIDO attestation intermediates as DER blobs.

    These are **not** trust anchors: they only bridge an attestation certificate up
    to a root from :func:`load_yubico_roots`. A YubiKey's packed attestation ships
    only its end-entity certificate in ``x5c`` (confirmed on firmware 5.8.0), while
    that certificate sits two tiers below the root, so without these the chain
    cannot be pinned at all.

    Needs only ``cryptography``, so it works without the ``fido2`` extra.
    """
    pem = Path(path) if path is not None else YUBICO_INTERMEDIATES_PEM
    return _load_pem_bundle(pem, what="Yubico intermediate certificates")


def _load_der(der: bytes, *, what: str = "certificate") -> x509.Certificate:
    try:
        return x509.load_der_x509_certificate(bytes(der))
    except Exception as e:  # noqa: BLE001
        raise AttestationError(f"cannot parse {what}: {e}") from e


def verify_certificate_chain(
    chain: Sequence[bytes],
    roots: Sequence[bytes],
    intermediates: Sequence[bytes] = (),
) -> x509.Certificate:
    """Verify ``chain`` (leaf first, DER) terminates at one of ``roots``.

    Checks each certificate is directly issued by the next, then that the top of the
    chain is issued by (or *is*) a trusted root. Returns the matching root.

    ``intermediates`` is an optional pool of CA certificates used to *bridge* a chain
    that stops short of a root — necessary because a YubiKey's attestation ships only
    its end-entity certificate while that certificate sits two tiers below the root.
    Each bridging certificate's signature is verified exactly like a supplied link, so
    this widens the set of *paths* that can be built, never the set of trust anchors:
    the top of the built path must still be issued by a member of ``roots``. Handing
    over an intermediate therefore confers no trust by itself. Certificates already
    present in ``chain`` are used from there; the pool only fills what is missing.

    Raises :class:`AttestationError` on an empty chain, an empty root set, a broken
    link, or no matching root — the message naming the issuer that could not be
    bridged. Note this deliberately does **not** do full RFC 5280 path validation (no
    revocation, no name constraints) — it pins the chain to a known root, which is
    what device attestation needs.
    """
    if not chain:
        raise AttestationError("attestation carries no certificate chain")
    if not roots:
        raise AttestationError("no trusted roots supplied to pin the chain against")

    certs = [_load_der(der, what="attestation certificate") for der in chain]
    for child, parent in zip(certs, certs[1:]):
        try:
            child.verify_directly_issued_by(parent)
        except Exception as e:  # noqa: BLE001
            raise AttestationError(
                f"broken certificate chain: {child.subject.rfc4514_string()} is not "
                f"issued by {parent.subject.rfc4514_string()} ({e})"
            ) from e

    root_certs = [_load_der(der, what="trusted root") for der in roots]
    pool = [_load_der(der, what="intermediate certificate") for der in intermediates]

    def _anchor_for(top: x509.Certificate) -> x509.Certificate | None:
        for root in root_certs:
            if top == root:
                return root  # the chain already includes the root itself
            try:
                top.verify_directly_issued_by(root)
            except Exception:  # noqa: BLE001 — wrong root, keep looking
                continue
            return root
        return None

    # Walk upward: anchor if we can, otherwise splice in one intermediate and retry.
    # Each pass either returns, breaks, or consumes a pool entry, so this terminates.
    while True:
        anchor = _anchor_for(certs[-1])
        if anchor is not None:
            return anchor

        bridge = None
        for i, candidate in enumerate(pool):
            if candidate.subject != certs[-1].issuer:
                continue  # cheap filter: an issuer's subject is the child's issuer
            try:
                certs[-1].verify_directly_issued_by(candidate)
            except Exception:  # noqa: BLE001 — not the real issuer, keep looking
                continue
            bridge = i
            break
        if bridge is None:
            break
        certs.append(pool.pop(bridge))

    raise AttestationError(
        f"certificate chain does not terminate at a trusted root "
        f"(top issuer: {certs[-1].issuer.rfc4514_string()})"
    )


def identify_yubico_certificate(cert_der: bytes) -> tuple[bool, str]:
    """Does this attestation certificate name Yubico as vendor/issuer?

    Returns ``(matched, detail)``. A name check alone proves nothing — always pair
    it with :func:`verify_certificate_chain`; this exists to give a human-readable
    reason and to catch an obviously foreign authenticator early.
    """
    cert = _load_der(cert_der)
    subject = cert.subject.rfc4514_string()
    issuer = cert.issuer.rfc4514_string()
    for label, name in (("subject", subject), ("issuer", issuer)):
        if "yubico" in name.lower():
            return True, f"{label} names Yubico: {name}"
    return False, f"neither subject ({subject}) nor issuer ({issuer}) names Yubico"


def verify_attestation_object(
    attestation_object: Any,
    client_data_hash: bytes,
    *,
    roots: Sequence[bytes] | None = None,
    require_root: bool = True,
    allowed_aaguids: Iterable[bytes] | None = None,
    intermediates: Sequence[bytes] | None = None,
) -> AttestationCheck:
    """Check an attestation object and decide whether it came from a YubiKey.

    Three independent gates, all of which must pass for ``is_yubikey``:

    * the attestation statement verifies over ``authData || client_data_hash``
      (so the device really holds the attestation key);
    * the certificate chain pins to a Yubico root (``roots``, defaulting to the
      bundled anchors; set ``require_root=False`` to skip — e.g. for a dev key);
    * the attestation certificate names Yubico.

    ``intermediates`` bridges the chain to a root and defaults to the bundled Yubico
    intermediates, which is required in practice: the device ships only its end-entity
    certificate. It adds no trust of its own (see :func:`verify_certificate_chain`).

    ``allowed_aaguids`` optionally narrows it further to specific models. It is
    deliberately not defaulted to a hardcoded list: Yubico ships new AAGUIDs with
    new hardware and treats the FIDO MDS as authoritative, so a baked-in list would
    reject future YubiKeys. The chain check is the durable identity signal.
    """
    require_fido2()
    from fido2.attestation import Attestation

    reasons: list[str] = []

    fmt = getattr(attestation_object, "fmt", "") or ""
    auth_data = getattr(attestation_object, "auth_data", None)
    credential_data = getattr(auth_data, "credential_data", None)
    aaguid = bytes(getattr(credential_data, "aaguid", b"") or b"")

    attestation_verified = False
    attestation_type: str | None = None
    trust_path: list[bytes] = []
    try:
        verifier = Attestation.for_type(fmt)()
        res = verifier.verify(attestation_object.att_stmt, auth_data, bytes(client_data_hash))
        attestation_verified = True
        attestation_type = getattr(res.attestation_type, "name", str(res.attestation_type))
        trust_path = list(res.trust_path or [])
    except Exception as e:  # noqa: BLE001 — every failure is "not trusted", uniformly
        reasons.append(f"attestation statement did not verify: {e}")

    subject = issuer = ""
    trusted_root_subject: str | None = None
    if trust_path:
        leaf = _load_der(trust_path[0], what="attestation certificate")
        subject = leaf.subject.rfc4514_string()
        issuer = leaf.issuer.rfc4514_string()

        matched, detail = identify_yubico_certificate(trust_path[0])
        if not matched:
            reasons.append(f"attestation certificate is not Yubico's: {detail}")

        if require_root:
            try:
                root = verify_certificate_chain(
                    trust_path,
                    list(roots) if roots is not None else load_yubico_roots(),
                    list(intermediates)
                    if intermediates is not None
                    else load_yubico_intermediates(),
                )
                trusted_root_subject = root.subject.rfc4514_string()
            except AttestationError as e:
                reasons.append(str(e))
    else:
        reasons.append(
            f"attestation format {fmt!r} carries no certificate chain, so the device "
            f"cannot be identified as a YubiKey"
        )

    if allowed_aaguids is not None and aaguid not in {bytes(a) for a in allowed_aaguids}:
        reasons.append(f"aaguid {aaguid.hex()} is not in the allowed set")

    return AttestationCheck(
        is_yubikey=attestation_verified and not reasons,
        attestation_verified=attestation_verified,
        reasons=tuple(reasons),
        aaguid=aaguid,
        fmt=fmt,
        attestation_type=attestation_type,
        certificate_subject=subject,
        certificate_issuer=issuer,
        trusted_root_subject=trusted_root_subject,
        chain_length=len(trust_path),
    )


def verify_yubikey_attestation(
    result: MakeCredentialResult,
    *,
    roots: Sequence[bytes] | None = None,
    require_root: bool = True,
    allowed_aaguids: Iterable[bytes] | None = None,
    use_seed_attestation: bool = False,
    intermediates: Sequence[bytes] | None = None,
) -> AttestationCheck:
    """Is the device behind ``result`` a genuine YubiKey?

    Checks the credential's own attestation by default. Pass
    ``use_seed_attestation=True`` to check the separate attestation that the
    ``previewSign`` extension returns for the generated ARKG seed key instead.
    """
    att_obj = result.seed_attestation_object if use_seed_attestation else result.attestation_object
    if att_obj is None:
        which = "seed" if use_seed_attestation else "credential"
        raise AttestationError(
            f"result carries no {which} attestation object - was make_credential() "
            f"run with attestation='none'?"
        )
    return verify_attestation_object(
        att_obj,
        result.client_data_hash,
        roots=roots,
        require_root=require_root,
        allowed_aaguids=allowed_aaguids,
        intermediates=intermediates,
    )


# --- step 5: sign with a derived key, and verify ---------------------------------
#: Signatures are ECDSA P-256 over SHA-256, so the digest handed to the device is 32B.
DIGEST_BYTES = 32


def signing_digest(*, data: bytes | None = None, digest: bytes | None = None) -> bytes:
    """The ``tbs`` ("to be signed") value the authenticator expects — a SHA-256 digest.

    Pass exactly one of ``data`` (hashed here) or ``digest`` (already hashed, e.g. you
    only ever had the hash). The extension signs this digest *as-is*: it does not hash
    again. That is why :func:`verify_signature` must use a pre-hashed verification when
    given a digest — hashing twice would silently never verify.
    """
    if (data is None) == (digest is None):
        raise ValueError("pass exactly one of data= or digest=")

    if digest is not None:
        if not isinstance(digest, (bytes, bytearray, memoryview)):
            raise TypeError(f"digest must be bytes, got {type(digest).__name__}")
        if len(digest) != DIGEST_BYTES:
            raise ValueError(
                f"digest must be a {DIGEST_BYTES}-byte SHA-256 hash, got {len(digest)} bytes"
            )
        return bytes(digest)

    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise TypeError(f"data must be bytes, got {type(data).__name__}")
    import hashlib

    return hashlib.sha256(bytes(data)).digest()


def build_sign_extension_input(
    *, credential_id: bytes, key_handle: bytes, tbs: bytes, arkg_args_cbor: bytes
) -> dict:
    """The ``previewSign.signByCredential`` extension input for ``get_assertion``.

    Split out as a pure function so the wire shape is testable without a device.
    """
    require_fido2()
    from fido2.utils import websafe_encode

    return {
        PREVIEW_SIGN_NAME: {
            "signByCredential": {
                websafe_encode(bytes(credential_id)): {
                    "keyHandle": bytes(key_handle),
                    "tbs": bytes(tbs),
                    "additionalArgs": bytes(arkg_args_cbor),
                }
            }
        }
    }


def _public_key_of(key: Any) -> Any:
    """Accept a :class:`DerivedKey` or a bare COSE key and return the COSE key."""
    return key.derived_public_key if isinstance(key, DerivedKey) else key


def p256_public_b64(public_key: Any) -> str:
    """A derived ARKG public key as the base64 point ``lib.crypto``'s ``p256`` speaks.

    Takes a :class:`DerivedKey` or the COSE key itself and returns the 33-byte
    compressed SEC1 point, base64-encoded — exactly the encoding
    :func:`lib.crypto.verify` expects for ``key_type="p256"``. That makes a derived
    key registrable through the §6 allowlist and verifiable by ``hook.py`` with no
    YubiKey-specific code on the verifying side: the authenticator's signature is
    ECDSA-P256 over SHA-256 in DER, which is the same scheme.

    Needs only ``cryptography`` (COSE coordinates in, base64 out), so it works
    without the ``fido2`` extra. Raises :class:`YubiKeyError` for anything that is
    not a usable P-256 public key — registration must fail loudly rather than
    publish a key no verifier could ever match.
    """
    from cryptography.hazmat.primitives.asymmetric import ec as _ec
    from cryptography.hazmat.primitives.serialization import PublicFormat

    cose = _public_key_of(public_key)
    try:
        point = _ec.EllipticCurvePublicNumbers(
            int.from_bytes(bytes(cose[-2]), "big"),
            int.from_bytes(bytes(cose[-3]), "big"),
            _ec.SECP256R1(),
        ).public_key()
    except (KeyError, IndexError, TypeError, ValueError) as e:
        raise YubiKeyError(f"not a usable P-256 public key: {e}") from e
    return _b64(point.public_bytes(Encoding.X962, PublicFormat.CompressedPoint))


def verify_signature(
    public_key: Any,
    signature: bytes,
    *,
    data: bytes | None = None,
    digest: bytes | None = None,
) -> bool:
    """Verify ``signature`` under a derived ARKG public key. Never raises.

    ``public_key`` may be a :class:`DerivedKey` or the COSE key itself. Pass exactly
    one of ``data`` (the original bytes) or ``digest`` (its SHA-256, when the original
    is not at hand); both paths accept the same signature.

    Fail-safe like :func:`lib.crypto.verify`: any malformed key, signature or input
    returns False rather than raising, so a caller can treat "did not verify" and
    "could not be verified" identically.
    """
    if (data is None) == (digest is None):
        raise ValueError("pass exactly one of data= or digest=")

    require_fido2()
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes as _hashes
    from cryptography.hazmat.primitives.asymmetric import ec as _ec
    from cryptography.hazmat.primitives.asymmetric.utils import Prehashed

    # One verification call for both paths. With a digest we must say Prehashed,
    # otherwise cryptography would hash the digest again and nothing would ever verify
    # (the extension signs the digest as-is — see signing_digest).
    if data is not None:
        payload, algorithm = data, _ec.ECDSA(_hashes.SHA256())
    else:
        payload, algorithm = digest, _ec.ECDSA(Prehashed(_hashes.SHA256()))

    cose = _public_key_of(public_key)
    try:
        # Rebuild the point ourselves rather than using CoseKey.verify, which always
        # hashes and so cannot serve the digest path.
        crypto_key = _ec.EllipticCurvePublicNumbers(
            int.from_bytes(bytes(cose[-2]), "big"),
            int.from_bytes(bytes(cose[-3]), "big"),
            _ec.SECP256R1(),
        ).public_key()
        crypto_key.verify(bytes(signature), bytes(payload), algorithm)
        return True
    except (InvalidSignature, ValueError, TypeError, KeyError, IndexError):
        return False


def credential_data_from_result(result: MakeCredentialResult) -> Any:
    """The credential's ``AttestedCredentialData``, needed to build allowCredentials.

    Read out of the attestation object — which is present even for
    ``attestation="none"`` (only the statement is empty, ``authData`` is not).
    """
    require_fido2()
    att_obj = result.attestation_object
    if att_obj is None:
        raise ExtensionUnsupported(
            "signing needs the credential's authenticator data, which travels in the "
            "attestation object — this result has none"
        )
    credential_data = att_obj.auth_data.credential_data
    if credential_data is None:
        raise ExtensionUnsupported("attestation object carries no credential data")
    return credential_data


def sign_with_derived_key(
    result: MakeCredentialResult,
    derived: DerivedKey,
    *,
    data: bytes | None = None,
    digest: bytes | None = None,
    rp_id: str = "example.com",
    rp_name: str = "ai-remote",
    origin: str | None = None,
    user_verification: str = "discouraged",
    device: Any = None,
    user_interaction: Any = None,
) -> bytes:
    """Have the authenticator sign with the ARKG private key matching ``derived``.

    Touches hardware: this is a ``getAssertion`` and the operator has to tap the key.
    The device is handed the ``key_handle`` and the ``arkg_args`` from the derivation —
    that pair is what lets it reconstruct the private half; nothing secret is stored
    off-device.

    Pass exactly one of ``data`` or ``digest`` (see :func:`signing_digest`). Returns the
    raw signature bytes, verifiable with :func:`verify_signature` against
    ``derived.derived_public_key``.
    """
    require_fido2()
    from fido2.ctap2.extensions import PreviewSignExtension
    from fido2.server import Fido2Server

    tbs = signing_digest(data=data, digest=digest)
    credential_data = credential_data_from_result(result)

    client, _info = _get_client(
        origin=origin or f"https://{rp_id}", device=device, user_interaction=user_interaction
    )
    server = Fido2Server({"id": rp_id, "name": rp_name})
    request_options, _state = server.authenticate_begin(
        [credential_data], user_verification=user_verification
    )

    response = client.get_assertion(
        {
            **request_options["publicKey"],
            "extensions": build_sign_extension_input(
                credential_id=result.credential_id,
                key_handle=derived.key_handle,
                tbs=tbs,
                arkg_args_cbor=derived.arkg_args_cbor,
            ),
        }
    )

    assertion = response.get_response(0)
    outputs = assertion.client_extension_results
    sign_output = getattr(outputs, PREVIEW_SIGN_NAME, None)
    if sign_output is None and isinstance(outputs, Mapping):
        sign_output = outputs.get(PreviewSignExtension.NAME)
    signature = getattr(sign_output, "signature", None)
    if signature is None and isinstance(sign_output, Mapping):
        signature = sign_output.get("signature")
    if not signature:
        raise ExtensionUnsupported(
            f"the authenticator returned no {PREVIEW_SIGN_NAME} signature "
            f"(extension outputs: {outputs!r}). On Windows this usually means the "
            f"native WebAuthn path was used - retry from an administrator terminal."
        )
    return bytes(signature)


# --- persisting a result between runs ------------------------------------------
#: Schema version of the JSON written by :func:`save_result`.
RESULT_FORMAT_VERSION = 1

# Fields of MakeCredentialResult that are raw bytes and travel as base64.
_RESULT_BYTES_FIELDS = (
    "key_handle",
    "seed_public_key_cbor",
    "credential_id",
    "aaguid",
    "client_data_hash",
)


def _b64(raw: bytes) -> str:
    return base64.b64encode(bytes(raw)).decode("ascii")


def _unb64(text: str, *, what: str) -> bytes:
    try:
        return base64.b64decode(text, validate=True)
    except Exception as e:  # noqa: BLE001
        raise YubiKeyError(f"{what} is not valid base64: {e}") from e


def result_to_dict(result: MakeCredentialResult) -> dict:
    """Serialise a :class:`MakeCredentialResult` to JSON-safe primitives.

    Lets ``make_credential`` (which needs the hardware and a touch) and
    ``seed_public_key`` (which needs neither) run as separate invocations — the
    whole point of the ``yubikey-exec make-credential`` / ``derive`` split.

    ``registration_response`` is intentionally dropped: it is a live fido2 object,
    not data, and nothing downstream needs it.
    """
    data: dict[str, Any] = {"v": RESULT_FORMAT_VERSION, "algorithm": result.algorithm}
    for name in _RESULT_BYTES_FIELDS:
        data[name] = _b64(getattr(result, name))
    for name in ("attestation_object", "seed_attestation_object"):
        obj = getattr(result, name)
        # AttestationObject is a bytes subclass, so this keeps the exact CBOR.
        data[name] = _b64(bytes(obj)) if obj is not None else None
    return data


def result_from_dict(data: Mapping[str, Any]) -> MakeCredentialResult:
    """Rebuild a :class:`MakeCredentialResult` from :func:`result_to_dict` output."""
    require_fido2()
    from fido2.webauthn import AttestationObject

    if not isinstance(data, Mapping):
        raise YubiKeyError("saved credential must be a JSON object")
    version = data.get("v")
    if version != RESULT_FORMAT_VERSION:
        raise YubiKeyError(
            f"saved credential has format version {version!r}, expected {RESULT_FORMAT_VERSION}"
        )

    fields: dict[str, Any] = {}
    for name in _RESULT_BYTES_FIELDS:
        value = data.get(name)
        if not isinstance(value, str):
            raise YubiKeyError(f"saved credential is missing {name!r}")
        fields[name] = _unb64(value, what=name)

    algorithm = data.get("algorithm")
    if not isinstance(algorithm, int) or isinstance(algorithm, bool):
        raise YubiKeyError("saved credential is missing a numeric 'algorithm'")

    att: dict[str, Any] = {}
    for name in ("attestation_object", "seed_attestation_object"):
        value = data.get(name)
        if value is None:
            att[name] = None
        elif isinstance(value, str):
            try:
                att[name] = AttestationObject(_unb64(value, what=name))
            except YubiKeyError:
                raise
            except Exception as e:  # noqa: BLE001
                raise YubiKeyError(f"saved {name} is not a readable attestation object: {e}") from e
        else:
            raise YubiKeyError(f"saved {name!r} must be a base64 string or null")

    return MakeCredentialResult(algorithm=algorithm, **fields, **att)


def save_result(result: MakeCredentialResult, path: str | os.PathLike[str]) -> None:
    """Write ``result`` to ``path`` as JSON (see :func:`result_to_dict`).

    Contains no private key material — the ARKG private halves never leave the
    authenticator — but it does identify the credential, so treat it as you would
    any credential handle.
    """
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(result_to_dict(result), indent=2, sort_keys=True), encoding="utf-8")


def load_result(path: str | os.PathLike[str]) -> MakeCredentialResult:
    """Read back a :func:`save_result` file."""
    p = Path(path)
    try:
        raw = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise YubiKeyError(f"cannot read saved credential {p}: {e}") from e
    return result_from_dict(raw)


# --- talking to the human in front of the key ----------------------------------
def console_user_interaction(*, touch_message: str = "\n>>> touch your YubiKey now <<<\n"):
    """A ``fido2.client.UserInteraction`` that prompts for touch / PIN on the console.

    Shared by both front ends (``tools/yubikey_exec.py`` and
    ``approver/responder_yubikey.py``) so a touch is announced the same way
    everywhere. Prompts go to stderr, leaving stdout free for machine-readable output.
    """
    require_fido2()
    from fido2.client import UserInteraction

    class _Console(UserInteraction):
        def prompt_up(self):
            print(touch_message, file=sys.stderr)

        def request_pin(self, permissions, rd_id):
            from getpass import getpass

            return getpass("enter YubiKey PIN: ")

        def request_uv(self, permissions, rd_id):
            print("user verification required", file=sys.stderr)
            return True

    return _Console()


# --- device access -------------------------------------------------------------
def enumerate_devices() -> Iterator[Any]:
    """Yield connected CTAP devices — USB HID first, then NFC readers if available."""
    require_fido2()
    from fido2.hid import CtapHidDevice

    yield from CtapHidDevice.list_devices()
    try:  # NFC is optional (needs pyscard); absence is not an error
        from fido2.pcsc import CtapPcscDevice
    except ImportError:
        return
    yield from CtapPcscDevice.list_devices()


def get_device_info(*, device: Any = None) -> DeviceInfo:
    """Read ``getInfo`` from ``device`` (or the first one found). No PIN, no touch."""
    require_fido2()
    from fido2.ctap2 import Ctap2

    dev = device
    if dev is None:
        dev = next(enumerate_devices(), None)
    if dev is None:
        raise no_authenticator_error()
    return device_info_from_info(Ctap2(dev).get_info())


def get_version(*, device: Any = None) -> FirmwareVersion:
    """The YubiKey's firmware version, e.g. ``5.8.0``.

    :attr:`FirmwareVersion.is_known` is False if the device does not report one
    (older CTAP2.0 firmware omits the field).
    """
    return get_device_info(device=device).firmware_version


def _get_client(*, origin: str, device: Any = None, user_interaction: Any = None):
    """Build a Fido2Client on a device that supports previewSign."""
    require_fido2()
    from fido2.client import DefaultClientDataCollector, Fido2Client
    from fido2.ctap2.extensions import PreviewSignExtension

    collector = DefaultClientDataCollector(origin)
    devices = [device] if device is not None else list(enumerate_devices())
    if not devices:
        raise no_authenticator_error()

    for dev in devices:
        client = Fido2Client(
            dev,
            client_data_collector=collector,
            user_interaction=user_interaction,
            extensions=[PreviewSignExtension()],
        )
        if supports_preview_sign(client.info):
            return client, client.info

    raise ExtensionUnsupported(
        f"no connected authenticator advertises the {PREVIEW_SIGN_NAME!r} extension - "
        f"ARKG needs YubiKey firmware {MIN_ARKG_FIRMWARE}+. On Windows, also make sure "
        f"you are running from an administrator terminal, otherwise the native WebAuthn "
        f"path is used and the extension output is dropped."
    )


def make_credential(
    *,
    rp_id: str = "example.com",
    rp_name: str = "ai-remote",
    origin: str | None = None,
    user_id: bytes = b"ai-remote-user",
    user_name: str = "ai-remote",
    user_verification: str = "discouraged",
    resident_key: str = "discouraged",
    attestation: str = "direct",
    device: Any = None,
    user_interaction: Any = None,
) -> MakeCredentialResult:
    """Run ``makeCredential`` with ``previewSign``, asking for an ARKG seed key.

    Touches hardware: the operator has to tap the key (and may be asked for a PIN,
    via ``user_interaction`` — a ``fido2.client.UserInteraction``).

    ``attestation`` defaults to ``"direct"`` so the reply carries a certificate
    chain and :func:`verify_yubikey_attestation` has something to check; pass
    ``"none"`` if you do not care who made the device.

    On **Windows**, run from an administrator terminal. Without it, ``fido2`` falls
    back to the OS WebAuthn API, which silently drops the ``previewSign`` output —
    this raises :class:`ExtensionUnsupported` in that case rather than returning a
    half-built result.
    """
    require_fido2()
    from fido2.ctap2.extensions import PreviewSignExtension
    from fido2.server import Fido2Server

    client, _info = _get_client(
        origin=origin or f"https://{rp_id}", device=device, user_interaction=user_interaction
    )

    server = Fido2Server({"id": rp_id, "name": rp_name}, attestation=attestation)
    create_options, _state = server.register_begin(
        {"id": user_id, "name": user_name},
        resident_key_requirement=resident_key,
        user_verification=user_verification,
        authenticator_attachment="cross-platform",
    )

    response = client.make_credential(
        {
            **create_options["publicKey"],
            "extensions": {
                PreviewSignExtension.NAME: {
                    "generateKey": {"algorithms": [_ARKG_SPLIT_ALG]}
                }
            },
        }
    )

    outputs = response.client_extension_results
    sign_output = getattr(outputs, PREVIEW_SIGN_NAME, None)
    if sign_output is None and isinstance(outputs, Mapping):
        sign_output = outputs.get(PREVIEW_SIGN_NAME)
    generated = getattr(sign_output, "generated_key", None)
    if generated is None:
        raise ExtensionUnsupported(
            f"the authenticator returned no {PREVIEW_SIGN_NAME} generated key "
            f"(extension outputs: {outputs!r}). On Windows this usually means the "
            f"native WebAuthn path was used - retry from an administrator terminal."
        )

    att_obj = response.response.attestation_object
    credential_data = att_obj.auth_data.credential_data
    return MakeCredentialResult(
        key_handle=bytes(generated.key_handle),
        seed_public_key_cbor=bytes(generated.public_key),
        algorithm=generated.algorithm,
        credential_id=bytes(credential_data.credential_id),
        aaguid=bytes(credential_data.aaguid),
        client_data_hash=bytes(response.response.client_data.hash),
        attestation_object=att_obj,
        registration_response=response,
        seed_attestation_object=getattr(generated, "attestation_object", None),
    )

