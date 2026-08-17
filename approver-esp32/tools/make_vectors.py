"""make_vectors.py -- generate the cross-language parity vectors (CLAUDE.md §10.11 tier 2).

The firmware and `approver/protocol.py` have to agree on exact byte strings, and
the failure when they stop agreeing is invisible from the device's own side: the
hook rejects every reply and Claude Code keeps asking in its own terminal, which
looks exactly like a responder that is simply not answering. So the expectations
the host tests compare against are **produced by the Python implementation
itself** and compiled in, never typed out of a design document.

That was already true of a handful of string literals pasted into
`test_signing.cpp`, `test_registration.cpp` and `components/crypto/device_key.cpp`.
What a pasted literal cannot be is *checked for staleness* -- it goes on passing
forever after `protocol.py` changes underneath it, which is the one thing this
tier exists to prevent. Hence a generator and two generated headers:

    host_test/vectors/parity_vectors.h    §7's decision bytes and §6's reply bytes
    components/crypto/selftest_vector.h   the Ed25519 vector §10.6's boot self-test consumes

Both are **committed**, the way `dependencies.lock` and `uv.lock` are: a fresh
checkout builds and tests with no Python step. What keeps them honest is
`tests/test_esp32_vectors.py`, which regenerates into memory on every
`pytest` run and fails if the committed files are not what today's `protocol.py`
and `lib/crypto.py` produce. The generator is one half of the tier; that test is
the other, and neither is worth much alone.

Run it (the venv, not the `py` launcher -- `lib/crypto.py` needs `cryptography`):

    .venv\\Scripts\\python.exe approver-esp32\\tools\\make_vectors.py

`--check` writes nothing and exits 1 if the files on disk are stale, which is
what the pytest guard reports in one line.
"""
from __future__ import annotations

import argparse
import base64
import sys
from pathlib import Path

# Non-package project, and this one is two directories down: make repo-root
# imports work when run directly as a script (root CLAUDE.md §2).
_REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO))

from approver import protocol  # noqa: E402
from lib import crypto  # noqa: E402

PARITY_HEADER = _REPO / "approver-esp32" / "host_test" / "vectors" / "parity_vectors.h"
SELFTEST_HEADER = _REPO / "approver-esp32" / "components" / "crypto" / "selftest_vector.h"

#: Repeated in the firmware as `protocol::kSessionIdMax` and friends (`signing.h`).
#: Kept here so a vector that would not fit the device's buffers is caught while
#: it is being generated rather than by a refusal in a test nobody expected.
SESSION_ID_MAX = 63
NONCE_MAX = 47
TOOL_NAME_MAX = 31
SHA256_HEX_MAX = 64
KEY_ID_MAX = 47
ERROR_MAX = 95

#: The self-test vector's seed is `00 01 02 … 1f` so that it is obviously a test
#: key and not something anybody could mistake for an identity (§10.6).
SELFTEST_SEED = bytes(range(32))

#: **Deliberately not shaped like anything signable.** §10.6 requires the boot
#: self-test not to be a signing oracle: this begins with a letter where §7's
#: signing bytes begin with the version digit and a separator, and it contains no
#: `\n` at all, so nothing the console prints from it can be rearranged into a
#: verdict.
SELFTEST_MESSAGE = b"ai-remote approver-esp32 libsodium self-test v1"


# --- the vectors ----------------------------------------------------------------
# Each is a case somebody has to be able to point at. The realistic ones prove the
# layout; the extreme ones prove the arithmetic, which is the half that fails
# silently.

def decision_cases() -> list[dict]:
    """§7's signing bytes -- the inputs, with the expectation computed below."""
    uuid = "4f9a2c1e-77b3-4d0a-9f21-8c6e5b3a1d02"
    nonce = "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI="
    sha = "e8b70996597e349ff8cb37b6cbe3ae52f6c64e27bfb3f41edf819c4817501f14"

    return [
        # What a real `Bash` approval looks like, end to end.
        dict(name="allow-bash", v=1, session_id=uuid, nonce=nonce, tool_name="Bash",
             input_sha256=sha, behavior="allow", ts=1737345600),
        # A deny, and a `ts` before the epoch -- the sign is the thing being pinned.
        dict(name="deny-negative-ts", v=1, session_id="s", nonce="n", tool_name="Write",
             input_sha256="0" * 64, behavior="deny", ts=-1),
        # The two values a `double` cannot carry. A float path loses the low bits
        # here and nowhere a human would ever notice.
        dict(name="allow-max-ts", v=1, session_id="s", nonce="n", tool_name="Bash",
             input_sha256="a" * 64, behavior="allow", ts=(1 << 63) - 1),
        dict(name="deny-min-ts", v=1, session_id="s", nonce="n", tool_name="Bash",
             input_sha256="f" * 64, behavior="deny", ts=-(1 << 63)),
        # **The largest message §7 can produce**, and therefore the one that says
        # `kSigningBytesMax` is the right number: every field at the bound the
        # firmware declares, and both integers at their widest spelling -- which is
        # the negative one, because the sign is a character too.
        dict(name="allow-at-every-bound", v=-(1 << 31), session_id="s" * SESSION_ID_MAX,
             nonce="n" * NONCE_MAX, tool_name="t" * TOOL_NAME_MAX,
             input_sha256="a" * SHA256_HEX_MAX, behavior="allow", ts=-(1 << 63)),
        # **Bytes, not characters.** `approvals.*` is open on the LAN (§10.3), so a
        # `session_id` that is not a UUID is ordinary attacker-shaped traffic
        # (§10.10) -- and the device copies the field out of the JSON while Python
        # encodes it as utf-8. This is the case that says those are the same thing.
        dict(name="allow-non-ascii-session", v=1, session_id="сессия-é中",
             nonce="n", tool_name="Bash", input_sha256=sha, behavior="allow",
             ts=1737345600),
    ]


def registration_reply_cases() -> list[dict]:
    """§6's reply signing bytes -- the same idea, for the exchange that pins the key."""
    nonce = "Tm9uY2VOb25jZU5vbmNlTm9uY2VOb25jZU5vbmNlMTI="

    return [
        dict(name="ok", v=1, ok=True, key_id="approver-esp32", nonce=nonce,
             ts=1737345600, error=""),
        # A *signed* rejection, which is a perfectly good reply and the one an
        # operator sees when a token has already been spent.
        dict(name="rejected", v=1, ok=False, key_id="approver-esp32", nonce=nonce,
             ts=1737345600, error="token unknown"),
        # The shape a rejection takes before the handler has worked out whose it
        # was: no name, no message, and a `ts` on the wrong side of the epoch.
        dict(name="rejected-anonymous", v=1, ok=False, key_id="", nonce="n", ts=-1,
             error=""),
        dict(name="ok-max-ts", v=1, ok=True, key_id="approver-esp32", nonce=nonce,
             ts=(1 << 63) - 1, error=""),
        dict(name="rejected-at-every-bound", v=1, ok=False, key_id="k" * KEY_ID_MAX,
             nonce="n" * 44, ts=0, error="e" * ERROR_MAX),
        # **`error` is last because it is the only free-text field**, and this is
        # the vector that makes that load-bearing rather than decorative: a
        # separator inside it must stay unambiguous, which it only is at the tail.
        dict(name="rejected-error-with-a-separator", v=1, ok=False,
             key_id="approver-esp32", nonce="n", ts=1737345600,
             error="line one\nline two"),
    ]


# --- rendering ------------------------------------------------------------------

def c_string(data: bytes, indent: str = "        ") -> str:
    """One C++ string literal for ``data``, split after every separator.

    Split rather than run together because these are `\\n`-joined field lists, so
    one line per field is the form somebody can actually read against §7's table.
    Non-printable bytes go out as **octal** escapes: a hex escape in C++ is greedy
    and would swallow a following digit, which is a corruption a generator has no
    business being able to produce.
    """
    chunks: list[list[str]] = [[]]
    for byte in data:
        char = chr(byte)
        if char == '"':
            chunks[-1].append('\\"')
        elif char == "\\":
            chunks[-1].append("\\\\")
        elif char == "\n":
            chunks[-1].append("\\n")
            chunks.append([])
        elif char == "\t":
            chunks[-1].append("\\t")
        elif 0x20 <= byte < 0x7F:
            chunks[-1].append(char)
        else:
            chunks[-1].append(f"\\{byte:03o}")

    if chunks and not chunks[-1]:
        chunks.pop()
    if not chunks:
        return '""'
    return f"\n{indent}".join(f'"{"".join(chunk)}"' for chunk in chunks)


def c_int64(value: int) -> str:
    """An `int64_t` literal a compiler will take.

    `INT64_C(-9223372036854775808)` is not one: C++ reads it as unary minus applied
    to a constant that does not fit in a signed 64-bit type, which under the host
    tests' `/W4 /WX` is an error rather than a warning. `INT64_MIN` is the only
    spelling of that value, and this is where a generator has to know it.
    """
    if value == -(1 << 63):
        return "INT64_MIN"
    return f"INT64_C({value})"


def c_int32(value: int) -> str:
    """The same trap one width down, and here it is a hard error rather than a
    warning: `-2147483648` is a `long long` in C++, and narrowing one of those
    into an `int32_t` member inside a braced initialiser is ill-formed."""
    if value == -(1 << 31):
        return "INT32_MIN"
    return str(value)


def c_bytes(data: bytes, indent: str = "    ") -> str:
    """A brace-initialiser for a `uint8_t` array, eleven bytes to a line."""
    lines = []
    for start in range(0, len(data), 11):
        row = ", ".join(f"0x{b:02x}" for b in data[start:start + 11])
        lines.append(f"{indent}{row},")
    return "\n".join(lines)


_BANNER = """// GENERATED FILE -- do not edit by hand.
//
// Produced by `approver-esp32/tools/make_vectors.py` from the Python
// implementation itself (CLAUDE.md §10.11 tier 2). Edit the generator, run it,
// and commit what it writes; `tests/test_esp32_vectors.py` fails if this file
// and today's Python disagree, so an edit made here is one somebody will have to
// undo.
"""


def render_parity_header() -> str:
    decisions = decision_cases()
    replies = registration_reply_cases()

    out = [_BANNER, "#pragma once", "", "#include <cstddef>", "#include <cstdint>",
           "#include <cstring>", "",
           "// §7's decision bytes and §6's registration-reply bytes, as the Python side",
           "// produces them. `test_vectors.cpp` runs the firmware's own assemblers against",
           "// every case here; the other host suites use `FindDecision` so that a layout",
           "// lives in exactly one place.",
           "", "namespace vectors {", ""]

    out += [
        "// One case of `approver/protocol.py::signing_bytes`. The fields are what goes",
        "// into `protocol::Decision`; `signing_bytes` is what must come out.",
        "struct DecisionVector {",
        "    const char *name;",
        "    int32_t v;",
        "    const char *session_id;",
        "    const char *nonce;",
        "    const char *tool_name;",
        "    const char *input_sha256;",
        "    const char *behavior;",
        "    int64_t ts;",
        "    const char *signing_bytes;",
        "    // **Carried rather than derived from the literal.** The signature is over",
        "    // bytes, so the length is part of the expectation and not a property of a",
        "    // terminator.",
        "    size_t signing_length;",
        "};",
        "",
        "inline constexpr DecisionVector kDecisions[] = {",
    ]

    for case in decisions:
        message = protocol.signing_bytes(
            v=case["v"], session_id=case["session_id"], nonce=case["nonce"],
            tool_name=case["tool_name"], input_sha256=case["input_sha256"],
            behavior=case["behavior"], updated_input_sha256="", ts=case["ts"], reason="")
        out += [
            "    {",
            f'        "{case["name"]}",',
            f"        {c_int32(case['v'])},",
            f"        {c_string(case['session_id'].encode('utf-8'))},",
            f"        {c_string(case['nonce'].encode('utf-8'))},",
            f"        {c_string(case['tool_name'].encode('utf-8'))},",
            f"        {c_string(case['input_sha256'].encode('utf-8'))},",
            f'        "{case["behavior"]}",',
            f"        {c_int64(case['ts'])},",
            f"        {c_string(message)},",
            f"        {len(message)},",
            "    },",
        ]

    out += [
        "};",
        "",
        "inline constexpr size_t kDecisionCount = sizeof kDecisions / sizeof kDecisions[0];",
        "",
    ]

    out += [
        "// One case of `approver/protocol.py::registration_reply_signing_bytes` (§6).",
        "struct ReplyVector {",
        "    const char *name;",
        "    int32_t v;",
        "    bool ok;",
        "    const char *key_id;",
        "    const char *nonce;",
        "    int64_t ts;",
        "    const char *error;",
        "    const char *signing_bytes;",
        "    size_t signing_length;",
        "};",
        "",
        "inline constexpr ReplyVector kRegistrationReplies[] = {",
    ]

    for case in replies:
        message = protocol.registration_reply_signing_bytes(
            v=case["v"], ok=case["ok"], key_id=case["key_id"], nonce=case["nonce"],
            ts=case["ts"], error=case["error"])
        out += [
            "    {",
            f'        "{case["name"]}",',
            f"        {c_int32(case['v'])},",
            f"        {'true' if case['ok'] else 'false'},",
            f"        {c_string(case['key_id'].encode('utf-8'))},",
            f"        {c_string(case['nonce'].encode('utf-8'))},",
            f"        {c_int64(case['ts'])},",
            f"        {c_string(case['error'].encode('utf-8'))},",
            f"        {c_string(message)},",
            f"        {len(message)},",
            "    },",
        ]

    out += [
        "};",
        "",
        "inline constexpr size_t kRegistrationReplyCount =",
        "    sizeof kRegistrationReplies / sizeof kRegistrationReplies[0];",
        "",
        "// Lookup by name, so a suite that wants one particular layout does not depend on",
        "// the order the generator happened to emit them in. Null when there is no such",
        "// case, which the caller asserts -- a renamed vector must fail loudly rather",
        "// than quietly test nothing.",
        "inline const DecisionVector *FindDecision(const char *name) {",
        "    for (const auto &vector : kDecisions) {",
        "        if (std::strcmp(vector.name, name) == 0) {",
        "            return &vector;",
        "        }",
        "    }",
        "    return nullptr;",
        "}",
        "",
        "}  // namespace vectors",
        "",
    ]
    return "\n".join(out)


def render_selftest_header() -> str:
    seed_b64 = base64.b64encode(SELFTEST_SEED).decode("ascii")
    pair = crypto.KeyPair.from_private_b64(seed_b64, crypto.ED25519)
    public_b64 = pair.public_b64()
    signature_b64 = pair.sign(SELFTEST_MESSAGE)

    # Generated and then checked with the module the hook itself verifies with.
    # A vector that `lib/crypto.py` would not accept is one the device could match
    # perfectly and still be rejected in the field.
    if not crypto.verify(public_b64, SELFTEST_MESSAGE, signature_b64, crypto.ED25519):
        raise SystemExit("lib/crypto.py will not verify the vector it just produced")

    public = base64.b64decode(public_b64)
    signature = base64.b64decode(signature_b64)

    return "\n".join([
        _BANNER,
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "// **The boot self-test's vector** (CLAUDE.md §10.6), produced by `lib/crypto.py`",
        "// -- the same module `hook.py` verifies real decisions with.",
        "//",
        "// It is one vector doing two jobs: `kSelfTestSignature` is the answer this",
        "// device must produce for `kSelfTestMessage`, *and* it is a Python-made",
        "// signature that this device's verify has to accept. A library that signs",
        "// correctly and verifies nothing would pass half of it.",
        "//",
        "// The seed is `00 01 02 … 1f` so that it is obviously a test key. The message",
        "// begins with a letter, where §7's signing bytes begin with the version digit",
        "// and a separator, and it holds no separator at all -- which is what keeps the",
        "// console command that prints it from being a signing oracle.",
        "",
        "namespace crypto {",
        "namespace vectors {",
        "",
        "inline constexpr uint8_t kSelfTestSeed[32] = {",
        c_bytes(SELFTEST_SEED),
        "};",
        "",
        "inline constexpr uint8_t kSelfTestPublicKey[32] = {",
        c_bytes(public),
        "};",
        "",
        f'inline constexpr char kSelfTestMessage[] = "{SELFTEST_MESSAGE.decode("ascii")}";',
        "",
        "inline constexpr uint8_t kSelfTestSignature[64] = {",
        c_bytes(signature),
        "};",
        "",
        "}  // namespace vectors",
        "}  // namespace crypto",
        "",
    ])


# --- the command ----------------------------------------------------------------

def generated() -> dict[Path, str]:
    """Every file this generator owns, rendered. The one entry point both the CLI
    and the pytest guard go through, so neither can check something the other does
    not write."""
    return {
        PARITY_HEADER: render_parity_header(),
        SELFTEST_HEADER: render_selftest_header(),
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="make_vectors.py", description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="write nothing; exit 1 if a file on disk is stale")
    args = parser.parse_args(argv)

    stale = []
    for path, text in generated().items():
        # Newlines are written explicitly rather than left to the platform: these
        # files are committed, and a generator that produced CRLF on Windows and LF
        # elsewhere would show every line as changed on the other machine.
        current = path.read_text(encoding="utf-8", newline="") if path.exists() else None
        if current == text:
            continue
        stale.append(path)
        if not args.check:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="")

    for path in stale:
        print(f"{'stale' if args.check else 'wrote'}: {path.relative_to(_REPO)}")
    if not stale:
        print("up to date")
    return 1 if (args.check and stale) else 0


if __name__ == "__main__":
    raise SystemExit(main())
