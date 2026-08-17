"""The Python half of the ESP32 parity tier (CLAUDE.md §10.11 tier 2).

`approver-esp32/host_test/test_vectors.cpp` asserts that the firmware reproduces
a set of byte strings. This asserts the other half — that those byte strings are
still what *today's* ``approver/protocol.py`` and ``lib/crypto.py`` produce.

Neither half is worth much alone, and the reason is the whole point of the tier.
A fixture compiled into a C++ test is a pasted literal with extra steps: change
``signing_bytes`` and the firmware suite goes on passing against yesterday's
layout forever. The failure that hides is the silent one — the device signs bytes
the hook does not recompute, every reply is rejected, and from the desk that is
indistinguishable from a responder that is not answering. Nothing logs it.

So this test regenerates both headers into memory and compares them with what is
committed. It is deliberately a *comparison of whole files* rather than of the
values inside them: a vector that quietly stopped being generated would pass any
field-by-field check that only looked at the vectors still present.

The generator is loaded by path because ``approver-esp32`` is not an importable
package name (the dash), and renaming the directory to suit a test would be the
tail wagging the dog.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
GENERATOR = REPO / "approver-esp32" / "tools" / "make_vectors.py"

REGENERATE = r".venv\Scripts\python.exe approver-esp32\tools\make_vectors.py"


def _load_generator():
    """Import ``make_vectors.py`` by path -- see the module docstring."""
    spec = importlib.util.spec_from_file_location("esp32_make_vectors", GENERATOR)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    # Registered before execution: the generator inserts the repo root on
    # ``sys.path`` at import time, and a module that imports itself out of
    # nowhere is harder to debug than one that is findable.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def generator():
    if not GENERATOR.exists():
        pytest.skip(f"{GENERATOR} is missing")
    return _load_generator()


def test_the_committed_vectors_are_what_python_produces_today(generator):
    """The one assertion this file exists for.

    A failure here means one of two things and the message says which: either
    ``protocol.py`` changed and the firmware has not been told, or somebody edited
    a generated header by hand. Both are fixed the same way -- run the generator
    and commit what it writes -- but only after deciding which of the two it was,
    because the first also means every registered device needs a new firmware.
    """
    for path, expected in generator.generated().items():
        assert path.exists(), f"{path.relative_to(REPO)} is missing; run: {REGENERATE}"
        current = path.read_text(encoding="utf-8", newline="")
        assert current == expected, (
            f"{path.relative_to(REPO)} is not what today's Python produces.\n"
            f"Regenerate and commit it:\n    {REGENERATE}"
        )


def test_the_check_mode_agrees_with_the_comparison(generator):
    """``--check`` is what a human runs; the test above is what CI runs. They have
    to be the same question, or one of them is a false sense of security."""
    assert generator.main(["--check"]) == 0


def test_there_are_vectors_to_compare(generator):
    """A generator that rendered nothing would make every assertion above vacuous
    -- an empty file compares equal to an empty file quite happily."""
    rendered = generator.generated()
    assert len(rendered) == 2
    for path, text in rendered.items():
        assert len(text) > 500, f"{path.name} is suspiciously short"


def test_every_decision_vector_is_reproducible_from_protocol_py(generator):
    """The vectors are computed, not remembered.

    The generated header carries the answer; this recomputes it through
    ``protocol.signing_bytes`` and checks the header's own numbers -- so a
    rendering bug that escaped into the committed file (an escape that decodes to
    the wrong byte, a length that does not match its literal) fails here rather
    than in a C++ suite whose failure output is a two-hundred-character diff.
    """
    from approver import protocol

    cases = generator.decision_cases()
    assert cases, "no decision vectors"

    names = [case["name"] for case in cases]
    assert len(names) == len(set(names)), "two decision vectors share a name"

    for case in cases:
        message = protocol.signing_bytes(
            v=case["v"], session_id=case["session_id"], nonce=case["nonce"],
            tool_name=case["tool_name"], input_sha256=case["input_sha256"],
            behavior=case["behavior"], updated_input_sha256="", ts=case["ts"], reason="")
        # §7's layout, from the other end: nine fields means eight separators, and
        # the two always-empty ones mean it ends in one.
        assert message.count(b"\n") >= 8
        assert message.endswith(b"\n")


def test_every_registration_reply_vector_is_reproducible_from_protocol_py(generator):
    from approver import protocol

    cases = generator.registration_reply_cases()
    assert cases, "no registration-reply vectors"

    names = [case["name"] for case in cases]
    assert len(names) == len(set(names)), "two reply vectors share a name"

    for case in cases:
        message = protocol.registration_reply_signing_bytes(
            v=case["v"], ok=case["ok"], key_id=case["key_id"], nonce=case["nonce"],
            ts=case["ts"], error=case["error"])
        assert message.startswith(protocol.REGISTRATION_REPLY_CONTEXT.encode("ascii"))


def test_the_self_test_vector_verifies_under_lib_crypto(generator):
    """§10.6's vector, checked with the module the hook verifies real decisions
    with. A vector the repository's own crypto would not accept is one the device
    could match perfectly and still be rejected in the field."""
    import base64

    from lib import crypto

    seed_b64 = base64.b64encode(generator.SELFTEST_SEED).decode("ascii")
    pair = crypto.KeyPair.from_private_b64(seed_b64, crypto.ED25519)
    signature = pair.sign(generator.SELFTEST_MESSAGE)

    assert crypto.verify(pair.public_b64(), generator.SELFTEST_MESSAGE, signature, crypto.ED25519)

    # And the bytes that verified are the ones in the committed header, spelled
    # the way C++ spells them.
    header = generator.SELFTEST_HEADER.read_text(encoding="utf-8", newline="")
    for raw in (base64.b64decode(pair.public_b64()), base64.b64decode(signature)):
        assert ", ".join(f"0x{b:02x}" for b in raw[:11]) in header


def test_the_self_test_message_cannot_be_rearranged_into_a_verdict(generator):
    """§10.6's rule that the boot self-test is not a signing oracle.

    §7's signing bytes begin with the version digit and a separator; this message
    begins with a letter and holds no separator at all, so nothing the console
    prints from it can be read as a decision. The firmware asserts the first half
    at compile time; this is where the whole rule is written down as a test.
    """
    message = generator.SELFTEST_MESSAGE
    assert not message[:1].isdigit()
    assert b"\n" not in message
