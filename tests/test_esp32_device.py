"""The device tier of CLAUDE.md §10.11 — a real board, a real bus, a real press.

**The acceptance test for the whole of `approver-esp32/`.** Everything else in
this repository can be green while the object on the desk is useless: the host
tier compiles the firmware's logic without the firmware, the parity tier checks
byte strings without a signature, and neither of them has ever seen the key that
is actually bound to that chip. This is the one test where §7 closes end to end —
a request `hook.build_request` made, a card a human read, a signature libsodium
produced on the ESP32, and `hook.verify_reply` saying `trusted` against the same
allowlist Claude Code uses.

§10.11 states the criterion in one sentence: *a device-signed reply that
``hook.verify_reply`` returns ``trusted=True`` for, plus the same reply with
``behavior`` flipped being rejected.* Both are below, and the second is free —
once a real signed reply exists, every tamper is an in-process assertion needing
no further press. That is why this file gets so much out of two interactions.

**Opt-in, and for a stronger reason than the YubiKey tier's.** That one blocks
on a finger; this one blocks on a finger *and* silently tests the wrong thing if
the board is not there. §6's queue group means each request reaches exactly one
responder, so with the device off and `responder.py serve` running, this suite
would pass beautifully against the software key. Hence
``AI_REMOTE_ESP32_DEVICE=1``, and hence the assertion on `key_id` — which is not
a formality here, it is what says who answered.

What it needs before it can pass:

* NATS up (CLAUDE.md §3) and the device connected to it — `nats` on the console;
* the device **registered**, so `approver-esp32` is in `handler-config.json`
  (§6, §10.7). It is not registered by this test: registration is a one-time
  exchange with a one-time token, and a test that spent one on every run would
  be a test that needs a fresh token on every run;
* nothing else subscribed to `approvals.*` — no `responder.py serve`, no browser
  tab. §6's "Multiple clients" is the first confusing symptom, and here it is a
  test that passes for the wrong device.

Run it:

    scripts\\esp32-approval.cmd

or by hand, and ``-s`` matters — without it pytest swallows the "press ALLOW
now" prompt and the run looks hung:

    $env:AI_REMOTE_ESP32_DEVICE="1"; .venv\\Scripts\\python.exe -m pytest tests/test_esp32_device.py -v -s
"""
from __future__ import annotations

import base64
import copy
import secrets
import sys
import time
from pathlib import Path

import pytest

from approver import hook
from lib import bus
from lib import config as configlib
from tests.conftest import requires_esp32_device, requires_nats, run_async

#: §10.2 — this device's own name in the allowlist, never shared with another
#: responder. Asserting it is how "the board answered" is told apart from
#: "something answered".
KEY_ID = "approver-esp32"

HANDLER_CONFIG = Path(__file__).resolve().parent.parent / "approver" / "handler-config.json"

#: Long enough for somebody to walk over, read a command and press a button.
PRESS_TIMEOUT = 90.0

#: And short enough that proving the fail-safe is not a coffee break. It only has
#: to outlast the device deciding *not* to answer, which it does by doing nothing.
SILENCE_TIMEOUT = 20.0

#: What the card asks about. Recognisable on the glass, and harmless if a stray
#: `allow` ever escaped to something that could run it.
COMMAND = "echo 'device tier: approver-esp32 acceptance test'"


def _prompt(text: str) -> None:
    """Say what the operator has to do, on stderr so ``-s`` shows it live.

    ASCII only: this lands on a Windows console with a legacy codepage, which is
    the same rule `tools/test_request.py` keeps.
    """
    print(f"\n    >>> {text}", file=sys.stderr, flush=True)


@pytest.fixture(scope="module")
def handler():
    """The real handler config -- the allowlist Claude Code would verify against.

    Deliberately not a fixture-built throwaway: a device-signed reply can only be
    verified against the key that device registered, so the point of this tier is
    that the allowlist is the live one.
    """
    if not HANDLER_CONFIG.exists():
        pytest.skip(f"{HANDLER_CONFIG} is missing -- nothing to verify against")

    cfg = configlib.Config.load(HANDLER_CONFIG, default={})
    data = {key: cfg[key] for key in cfg}
    allowlist = hook.allowlist_from_config(data)

    if KEY_ID not in allowlist:
        pytest.fail(
            f"{KEY_ID} is not in {HANDLER_CONFIG.name}: the board has not registered.\n"
            f"Mint a token and run `register <token>` on its console "
            f"(approver-esp32/commands.md)."
        )
    return {
        "allowlist": allowlist,
        "servers": hook.servers_from_config(data),
    }


def _ask(handler, *, timeout: float, command: str = COMMAND):
    """One §7 exchange. Returns ``(request, reply)``; raises on no answer."""
    payload = {
        "hook_event_name": hook.HOOK_EVENT,
        "session_id": "esp32-device-" + secrets.token_hex(4),
        "tool_name": "Bash",
        "tool_input": {"command": command},
        "permission_mode": "default",
        "cwd": str(Path.cwd()),
    }
    nonce = base64.b64encode(secrets.token_bytes(32)).decode("ascii")
    request = hook.build_request(payload, nonce=nonce, ts=int(time.time()))

    async def go():
        async with bus.connect(handler["servers"]) as b:
            return await b.request(
                f"approvals.{payload['session_id']}", request, timeout=timeout
            )

    return request, run_async(go())


# --- the one press this suite needs --------------------------------------------
@pytest.fixture(scope="module")
def allowed(handler):
    """A request answered on the board with ALLOW, and the reply it signed.

    Module-scoped on purpose: every assertion below is about *this* reply, and
    asking for a press per test would be a suite nobody runs twice.
    """
    _prompt("A card is about to appear on the device. Press ALLOW (the BOOT button).")
    try:
        request, reply = _ask(handler, timeout=PRESS_TIMEOUT)
    except bus.NoResponders:
        pytest.fail(
            "nobody is subscribed to approvals.* -- the device is off, not "
            "connected, or not registered. `nats` and `request` on its console say which."
        )
    except bus.RequestTimeout:
        pytest.fail(f"no reply within {PRESS_TIMEOUT:.0f}s -- was ALLOW pressed?")
    _prompt("thank you.")
    return request, reply


@requires_nats
@requires_esp32_device
def test_the_device_signed_a_reply_the_hook_trusts(handler, allowed):
    """**The acceptance criterion**, in the hook's own words.

    Not an approximation of it: `verify_reply` is what `hook.py` calls, against
    the allowlist `hook.py` reads, so a pass here is Claude Code acting on this
    board's press.
    """
    request, reply = allowed
    trusted, reason = hook.verify_reply(request, reply, handler["allowlist"])
    assert trusted, f"the hook would reject this device's reply: {reason}"


@requires_nats
@requires_esp32_device
def test_it_answered_as_itself(allowed):
    """§6's queue group hands a request to exactly one responder, so "something
    answered" and "the board answered" are different facts. Without this line the
    whole suite would pass against `responder.py serve`."""
    _, reply = allowed
    assert reply.get("key_id") == KEY_ID


@requires_nats
@requires_esp32_device
def test_the_verdict_is_the_button_that_was_pressed(allowed):
    _, reply = allowed
    assert reply.get("behavior") == "allow"


@requires_nats
@requires_esp32_device
def test_the_reply_carries_nothing_this_device_originates(allowed):
    """§10.2: no `updated_input`, and an empty `reason`.

    That is not a stylistic choice -- `updated_input` is the only field a
    responder originates, and carrying it would force this firmware to reproduce
    Python's canonical JSON and hash it identically. Its absence is what keeps
    `updated_input_sha256` empty in the signed bytes, so this assertion and the
    signature verifying are two halves of one claim.
    """
    _, reply = allowed
    assert "updated_input" not in reply or reply["updated_input"] is None
    assert reply.get("reason", "") == ""


# --- and every tamper, which needs no further press -----------------------------
@requires_nats
@requires_esp32_device
def test_the_same_reply_with_the_verdict_flipped_is_rejected(handler, allowed):
    """The second half of §10.11's criterion, and the one that says the signature
    is over the verdict rather than merely present next to it."""
    request, reply = allowed
    tampered = copy.deepcopy(reply)
    tampered["behavior"] = "deny"

    trusted, reason = hook.verify_reply(request, tampered, handler["allowlist"])
    assert not trusted, "a flipped verdict was accepted -- the signature covers nothing"
    assert "signature" in reason


@requires_nats
@requires_esp32_device
def test_every_echoed_field_binds_the_verdict_to_this_request(handler, allowed):
    """§7's echo fields, one at a time.

    Each is a different attack and they are worth separating: `input_sha256` is
    "a verdict for one command replayed onto another", `nonce` and `ts` are
    "yesterday's allow replayed today", `session_id` is "another session's
    answer". A reply that verified with any of them changed would be a reply that
    binds to nothing.
    """
    request, reply = allowed
    for field, wrong in (
        ("v", 2),
        ("session_id", "somebody-elses-session"),
        ("tool_name", "Write"),
        ("input_sha256", "0" * 64),
        ("nonce", base64.b64encode(secrets.token_bytes(32)).decode("ascii")),
        ("ts", request["ts"] + 1),
    ):
        tampered = copy.deepcopy(reply)
        tampered[field] = wrong
        trusted, reason = hook.verify_reply(request, tampered, handler["allowlist"])
        assert not trusted, f"a reply with a changed {field} was accepted"


@requires_nats
@requires_esp32_device
def test_a_reply_that_claims_another_responder_is_rejected(handler, allowed):
    """The device's `key_id` selects the key **and** the scheme on the verifying
    side (§7), so a reply that renames itself is checked against somebody else's
    key and fails. This is what stops a compromised responder borrowing another's
    slot in the allowlist."""
    request, reply = allowed
    tampered = copy.deepcopy(reply)
    tampered["key_id"] = "approver-1"

    trusted, _ = hook.verify_reply(request, tampered, handler["allowlist"])
    assert not trusted

    tampered["key_id"] = "no-such-responder"
    trusted, reason = hook.verify_reply(request, tampered, handler["allowlist"])
    assert not trusted
    assert "allowlist" in reason


@requires_nats
@requires_esp32_device
def test_a_reply_with_no_signature_at_all_is_rejected(handler, allowed):
    """The failure mode an unregistered responder produces, and the one that must
    never be read as "nothing was wrong with it"."""
    request, reply = allowed
    for missing in ({}, {"sig": ""}, {"sig": None}, {"sig": "not base64"}):
        tampered = copy.deepcopy(reply)
        tampered.pop("sig", None)
        tampered.update(missing)
        trusted, _ = hook.verify_reply(request, tampered, handler["allowlist"])
        assert not trusted


# --- the fail-safe, which is the other half of what this device is for ----------
@requires_nats
@requires_esp32_device
def test_pressing_nothing_sends_nothing(handler):
    """§10.10, on real hardware: **no reply is the safe outcome.**

    The card goes up, nobody touches it, and the device must publish *nothing* —
    not a deny, not a "skipped", not an empty verdict. The hook times out and
    Claude Code asks in its own terminal. This is deliberately the last test in
    the file: it is the one that costs the operator a wait rather than a press.

    A `RequestTimeout` here is the pass. Anything arriving is a failure severe
    enough to name what it was, because a device that answers on a timer is a
    device that approves things nobody read.
    """
    _prompt(
        f"A second card will appear. Press NOTHING for about {SILENCE_TIMEOUT:.0f}s "
        f"-- this checks that silence stays silence."
    )
    try:
        _, reply = _ask(handler, timeout=SILENCE_TIMEOUT,
                        command="echo 'device tier: nobody should answer this'")
    except bus.RequestTimeout:
        _prompt("nothing was sent, which is the correct answer.")
        return
    pytest.fail(f"the device answered a card nobody pressed: {reply!r}")
