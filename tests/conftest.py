"""Shared pytest helpers.

Some tests need something the machine may not have. Rather than failing, they skip:
we probe once at collection time and expose ready-made markers.

* NATS on 127.0.0.1:4222 (CLAUDE.md §3) -> ``requires_nats``
* a physical YubiKey with previewSign (CLAUDE.md §8) -> ``requires_yubikey`` /
  ``requires_preview_sign`` / ``requires_yubikey_touch``
* the ESP32 responder, powered on and registered (CLAUDE.md §10.11 tier 3) ->
  ``requires_esp32_device``

A plain ``py -m pytest`` therefore stays green on a bare checkout with no hardware.
"""
import asyncio
import os
import socket

import pytest

from lib.bus import DEFAULT_SERVERS

#: Set to 1 to allow tests that make the operator press the YubiKey button.
TOUCH_ENV = "AI_REMOTE_YUBIKEY_TOUCH"


def _nats_reachable(host: str = "127.0.0.1", port: int = 4222) -> bool:
    try:
        with socket.create_connection((host, port), timeout=1):
            return True
    except OSError:
        return False


requires_nats = pytest.mark.skipif(
    not _nats_reachable(),
    reason="NATS server not reachable on 127.0.0.1:4222 (see CLAUDE.md §3)",
)


def _probe_yubikey():
    """Return ``(device_present, preview_sign, detail)``. Never raises.

    Probed once per session: enumerating CTAP devices and reading getInfo needs no
    PIN and no touch, so this is safe to do at import time.
    """
    try:
        from lib import yubikey
    except Exception as e:  # noqa: BLE001 - a broken import must not break collection
        return False, False, f"lib.yubikey unimportable: {e}"

    if not yubikey.FIDO2_AVAILABLE:
        return False, False, "optional extra not installed (uv sync --extra yubikey)"
    try:
        info = yubikey.get_device_info()
    except yubikey.NoAuthenticator:
        return False, False, "no FIDO authenticator connected"
    except Exception as e:  # noqa: BLE001 - flaky USB/NFC enumeration is a skip, not a failure
        return False, False, f"could not read device info: {e}"
    return True, info.supports_preview_sign, f"firmware {info.firmware_version}"


_YUBIKEY_PRESENT, _PREVIEW_SIGN, _YUBIKEY_DETAIL = _probe_yubikey()

requires_yubikey = pytest.mark.skipif(
    not _YUBIKEY_PRESENT, reason=f"no usable YubiKey: {_YUBIKEY_DETAIL}"
)

requires_preview_sign = pytest.mark.skipif(
    not _PREVIEW_SIGN,
    reason=(
        f"YubiKey does not advertise previewSign ({_YUBIKEY_DETAIL}) — needs firmware "
        f"5.8.0+, and on Windows an administrator terminal"
    ),
)

#: Tests that block waiting for a finger. Opt-in, so an unattended `pytest` never hangs.
requires_yubikey_touch = pytest.mark.skipif(
    os.environ.get(TOUCH_ENV) != "1",
    reason=f"needs a physical button press; set {TOUCH_ENV}=1 to enable",
)

#: Set to 1 to allow the device tier of CLAUDE.md §10.11 -- the ESP32 responder
#: on the desk, answered by hand.
DEVICE_ENV = "AI_REMOTE_ESP32_DEVICE"

#: The ESP32 tier is opt-in for a reason the YubiKey one only half shares. That
#: one needs a finger; this one also needs a **board that is powered on,
#: registered and subscribed** -- and if it is not, the request goes onto
#: ``approvals.*`` where any other running responder will happily answer it. An
#: unattended `pytest` that quietly tested the software responder instead would
#: be worse than one that skipped.
requires_esp32_device = pytest.mark.skipif(
    os.environ.get(DEVICE_ENV) != "1",
    reason=(
        f"needs the ESP32 responder on the desk and a press on it; "
        f"set {DEVICE_ENV}=1 to enable (scripts\\esp32-approval.cmd does)"
    ),
)


def run_async(coro):
    """Drive an async coroutine to completion without pytest-asyncio (not an approved dep)."""
    return asyncio.run(coro)


__all__ = [
    "requires_nats",
    "requires_yubikey",
    "requires_preview_sign",
    "requires_yubikey_touch",
    "requires_esp32_device",
    "run_async",
    "DEFAULT_SERVERS",
    "TOUCH_ENV",
    "DEVICE_ENV",
]
