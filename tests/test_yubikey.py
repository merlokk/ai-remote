"""Tests for lib/yubikey.py that need neither `fido2` nor a physical YubiKey.

The module must be importable with the optional `fido2` extra absent (CLAUDE.md
§8), so everything here runs on a bare `uv sync`. The fido2-dependent half lives
in test_yubikey_fido2.py.
"""
import pytest

from lib import yubikey


# --- firmware version parsing --------------------------------------------------
@pytest.mark.parametrize(
    "raw, expected",
    [
        (0x050800, (5, 8, 0)),  # YubiKey 5.8.0 — the first firmware with previewSign
        (0x050506, (5, 5, 6)),
        (328966, (5, 5, 6)),  # same value, decimal — the form CTAP2 actually reports
        (0x040304, (4, 3, 4)),
        (0xFFFFFF, (255, 255, 255)),
    ],
)
def test_parse_firmware_version_decodes_packed_bytes(raw, expected):
    v = yubikey.parse_firmware_version(raw)
    assert v.as_tuple() == expected
    assert (v.major, v.minor, v.patch) == expected
    assert v.raw == raw


def test_parse_firmware_version_str_is_dotted():
    assert str(yubikey.parse_firmware_version(0x050800)) == "5.8.0"


def test_parse_firmware_version_zero_is_unknown():
    # CTAP2 getInfo defaults firmwareVersion to 0 when the field is absent.
    v = yubikey.parse_firmware_version(0)
    assert v.is_known is False
    assert v.as_tuple() == (0, 0, 0)


def test_parse_firmware_version_nonzero_is_known():
    assert yubikey.parse_firmware_version(0x050800).is_known is True


def test_firmware_versions_order_by_component():
    older = yubikey.parse_firmware_version(0x050704)
    newer = yubikey.parse_firmware_version(0x050800)
    assert older < newer
    assert newer > older
    assert older != newer


def test_firmware_version_equality_ignores_nothing_surprising():
    assert yubikey.parse_firmware_version(0x050800) == yubikey.parse_firmware_version(0x050800)


@pytest.mark.parametrize("bad", [-1, -0x050800])
def test_parse_firmware_version_rejects_negative(bad):
    with pytest.raises(ValueError):
        yubikey.parse_firmware_version(bad)


@pytest.mark.parametrize("bad", ["5.8.0", None, 5.8, True])
def test_parse_firmware_version_rejects_non_int(bad):
    with pytest.raises(TypeError):
        yubikey.parse_firmware_version(bad)


def test_parse_firmware_version_rejects_overlong():
    # More than three packed bytes is not a version we can decode unambiguously.
    with pytest.raises(ValueError):
        yubikey.parse_firmware_version(0x01000000)


def test_min_arkg_firmware_constant():
    # previewSign / ARKG needs 5.8.0 or later.
    assert yubikey.MIN_ARKG_FIRMWARE.as_tuple() == (5, 8, 0)


# --- previewSign support probing (pure, works on any info-like object) ---------
class _FakeInfo:
    def __init__(self, extensions=(), firmware_version=0, aaguid=b"\x00" * 16):
        self.extensions = list(extensions)
        self.firmware_version = firmware_version
        self.aaguid = aaguid
        self.versions = ["U2F_V2", "FIDO_2_0", "FIDO_2_1"]


def test_supports_preview_sign_true_when_extension_advertised():
    assert yubikey.supports_preview_sign(_FakeInfo(extensions=["credProtect", "previewSign"]))


def test_supports_preview_sign_false_when_absent():
    assert not yubikey.supports_preview_sign(_FakeInfo(extensions=["credProtect"]))


def test_supports_preview_sign_false_on_none():
    # get_client() returns info=None on the Windows WebAuthn path.
    assert not yubikey.supports_preview_sign(None)


def test_device_info_from_info_maps_fields():
    info = _FakeInfo(extensions=["previewSign"], firmware_version=0x050800, aaguid=b"\xab" * 16)
    dev = yubikey.device_info_from_info(info)
    assert dev.firmware_version.as_tuple() == (5, 8, 0)
    assert dev.aaguid == b"\xab" * 16
    assert dev.aaguid_hex == "ab" * 16
    assert dev.supports_preview_sign is True
    assert "previewSign" in dev.extensions
    assert "FIDO_2_1" in dev.versions


def test_device_info_reports_unknown_firmware():
    dev = yubikey.device_info_from_info(_FakeInfo())
    assert dev.firmware_version.is_known is False
    assert dev.supports_preview_sign is False


def test_device_info_meets_arkg_firmware():
    new = yubikey.device_info_from_info(_FakeInfo(extensions=["previewSign"], firmware_version=0x050800))
    old = yubikey.device_info_from_info(_FakeInfo(extensions=["previewSign"], firmware_version=0x050704))
    assert new.meets_arkg_firmware is True
    assert old.meets_arkg_firmware is False


# --- ikm / ctx validation (pure) -----------------------------------------------
def test_validate_ikm_accepts_32_bytes():
    yubikey.validate_ikm(b"\x01" * 32)  # no raise


def test_validate_ikm_accepts_longer():
    yubikey.validate_ikm(b"\x01" * 64)


@pytest.mark.parametrize("short", [b"", b"\x01" * 31])
def test_validate_ikm_rejects_under_256_bits(short):
    # The ARKG draft recommends >= 256 bits of entropy for ikm.
    with pytest.raises(ValueError):
        yubikey.validate_ikm(short)


def test_validate_ikm_rejects_non_bytes():
    with pytest.raises(TypeError):
        yubikey.validate_ikm("x" * 32)


def test_validate_ctx_rejects_non_bytes():
    with pytest.raises(TypeError):
        yubikey.validate_ctx("my-ctx")


def test_validate_ctx_rejects_empty():
    with pytest.raises(ValueError):
        yubikey.validate_ctx(b"")


def test_validate_ctx_accepts_bytes():
    yubikey.validate_ctx(b"my-ctx")


# --- optional-dependency contract ----------------------------------------------
def test_module_exposes_availability_flag():
    assert isinstance(yubikey.FIDO2_AVAILABLE, bool)


def test_error_hierarchy():
    assert issubclass(yubikey.Fido2NotInstalled, yubikey.YubiKeyError)
    assert issubclass(yubikey.NoAuthenticator, yubikey.YubiKeyError)
    assert issubclass(yubikey.ExtensionUnsupported, yubikey.YubiKeyError)
    assert issubclass(yubikey.AttestationError, yubikey.YubiKeyError)


def test_require_fido2_message_names_the_extra(monkeypatch):
    # Simulate the extra not being installed, whatever the real environment has.
    monkeypatch.setattr(yubikey, "FIDO2_AVAILABLE", False)
    monkeypatch.setattr(yubikey, "_IMPORT_ERROR", ModuleNotFoundError("No module named 'fido2'"))
    with pytest.raises(yubikey.Fido2NotInstalled) as e:
        yubikey.require_fido2()
    assert "yubikey" in str(e.value)  # the extra's name, so the hint is actionable


# --- "no authenticator" diagnostics -------------------------------------------
def test_no_authenticator_hint_mentions_elevation_on_unelevated_windows():
    # Windows hands FIDO HID devices to the WebAuthn API only; an unelevated process
    # sees an empty device list even with a key plugged in. Say so.
    hint = yubikey.no_authenticator_hint(is_windows=True, is_admin=False)
    assert "administrator" in hint.lower()


def test_no_authenticator_hint_on_elevated_windows_does_not_blame_elevation():
    hint = yubikey.no_authenticator_hint(is_windows=True, is_admin=True)
    assert "administrator" not in hint.lower()
    assert "plugged in" in hint.lower()


def test_no_authenticator_hint_off_windows_is_plain():
    hint = yubikey.no_authenticator_hint(is_windows=False, is_admin=False)
    assert "administrator" not in hint.lower()
    assert "plugged in" in hint.lower()


def test_no_authenticator_error_is_the_right_type():
    err = yubikey.no_authenticator_error()
    assert isinstance(err, yubikey.NoAuthenticator)
    assert str(err)


def test_yubico_root_bundle_file_exists():
    # Shipped as data next to the module; readable without fido2.
    assert yubikey.YUBICO_ROOTS_PEM.is_file()
    assert b"BEGIN CERTIFICATE" in yubikey.YUBICO_ROOTS_PEM.read_bytes()


def test_yubico_intermediate_bundle_file_exists():
    # Also data next to the module; needed because a YubiKey ships only its EE cert.
    assert yubikey.YUBICO_INTERMEDIATES_PEM.is_file()
    assert b"BEGIN CERTIFICATE" in yubikey.YUBICO_INTERMEDIATES_PEM.read_bytes()


def test_root_and_intermediate_bundles_are_separate_files():
    assert yubikey.YUBICO_ROOTS_PEM != yubikey.YUBICO_INTERMEDIATES_PEM
