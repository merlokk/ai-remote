"""test_request.py -- put one permission request on the bus and report the answer.

A responder-side test probe: it sends exactly what ``hook.py`` would send for a
tool call, waits for a responder to answer, and then judges the reply with
``hook.verify_reply`` -- the hook's own verifier, against the same
``handler-config.json`` allowlist. So the verdict it prints is the verdict Claude
Code would act on, not an approximation.

Nothing here is reimplemented: the request comes from ``hook.build_request`` and
the checks from ``hook.verify_reply``. If those change, this follows.

Useful for exercising a responder without a live Claude Code session -- the web
UI, ``responder.py serve``, or ``responder_yubikey.py serve`` all look the same
from here.

Run (see also scripts\\test-request.cmd):
  py tools/test_request.py --command "echo hello"

**--input quoting differs by shell, and the two are opposites** (verified on
Windows; ``--dry-run`` prints what actually arrived, so check there first):

  PowerShell:  --input '{"file_path": "x.txt"}'
  cmd.exe:     --input "{\\"file_path\\": \\"x.txt\\"}"

PowerShell does not pass the escaped-double-quote form through to a native
process, and cmd.exe does not treat single quotes as quoting at all -- each
shell's working form is the other's argparse error.

Exit codes: 0 = a trusted reply arrived, 1 = error / no reply, 3 = a reply
arrived but the hook would reject it.
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import json
import secrets
import sys
import time
from pathlib import Path

# Non-package project: make repo-root imports work when run directly as a script.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from approver import hook  # noqa: E402
from lib import bus  # noqa: E402
from lib import config as configlib  # noqa: E402

DEFAULT_CONFIG = Path(__file__).resolve().parent.parent / "approver" / "handler-config.json"
DEFAULT_TOOL = "Bash"
DEFAULT_COMMAND = "echo 'test request from tools/test_request.py'"

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_UNTRUSTED = 3


# --- pure helpers ---------------------------------------------------------------
def parse_tool_input(*, command: str | None, raw: str | None) -> dict:
    """Build ``tool_input``: either ``--input`` JSON, or ``--command`` shorthand.

    The shorthand exists because a Bash request is the common case and quoting a
    JSON object through cmd.exe is unpleasant.
    """
    if raw is not None:
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError as e:
            raise ValueError(f"--input is not valid JSON: {e}") from e
        if not isinstance(parsed, dict):
            raise ValueError("--input must be a JSON object")
        return parsed
    return {"command": command if command is not None else DEFAULT_COMMAND}


def build_payload(
    *,
    tool_name: str,
    tool_input: dict,
    session_id: str,
    cwd: str,
    permission_mode: str,
) -> dict:
    """A `PermissionRequest` hook payload, the shape ``hook.build_request`` takes."""
    return {
        "hook_event_name": hook.HOOK_EVENT,
        "session_id": session_id,
        "tool_name": tool_name,
        "tool_input": tool_input,
        "permission_mode": permission_mode,
        "cwd": cwd,
    }


def default_session_id() -> str:
    """A recognisable, unique session so the request cannot be confused with real traffic."""
    return "test-request-" + secrets.token_hex(4)


def describe_reply(reply: dict, *, trusted: bool, reason: str) -> dict:
    """Flatten a reply into the fields worth printing.

    Two digests come along:

    * ``signature_sha256`` -- the same fingerprint
      ``responder_yubikey.print_decision`` echoes on the operator's console, in
      the same ``sha256:<hex>`` shape, so the two lines can be compared
      character for character. That is the only way to confirm the decision that
      reached the hook is the one the operator watched themselves sign.
    * ``input_sha256`` -- echoed by the responder and already checked against the
      request by ``hook.verify_reply``; printing it shows *which command* the
      verdict is bound to (§7), not merely that a verdict arrived.
    """
    sig = reply.get("sig")
    signature_bytes = 0
    signature_sha256 = None
    if isinstance(sig, str) and sig:
        try:
            raw = base64.b64decode(sig, validate=True)
        except Exception:  # noqa: BLE001 -- a malformed sig is reported, not raised
            signature_bytes = -1
        else:
            signature_bytes = len(raw)
            signature_sha256 = hashlib.sha256(raw).hexdigest()
    return {
        "behavior": reply.get("behavior"),
        "reason": reply.get("reason", ""),
        "key_id": reply.get("key_id"),
        "input_sha256": reply.get("input_sha256"),
        "signature_bytes": signature_bytes,
        "signature_sha256": signature_sha256,
        "updated_input": reply.get("updated_input"),
        "trusted": trusted,
        "verdict_reason": reason,
    }


def format_report(report: dict) -> str:
    """Render :func:`describe_reply` output for a console. ASCII only (CLAUDE.md 8)."""
    lines = [
        f"  behavior  : {report['behavior']}",
        f"  reason    : {report['reason']!r}",
        f"  key_id    : {report['key_id']}",
        f"  input sha : {report['input_sha256']}",
    ]
    signature_bytes = report["signature_bytes"]
    if signature_bytes > 0:
        # Same shape as responder_yubikey.print_decision, so an operator can
        # compare this line with the one their own console printed.
        lines.append(f"  signature : sha256:{report['signature_sha256']} ({signature_bytes} bytes)")
    elif signature_bytes == 0:
        lines.append("  signature : NONE - an unregistered responder answered")
    else:
        lines.append("  signature : malformed (not base64)")
    if report.get("updated_input") is not None:
        lines.append(f"  updated   : {json.dumps(report['updated_input'], ensure_ascii=False)}")
    if report["trusted"]:
        lines.append(f"  verdict   : TRUSTED - Claude Code would {report['behavior']} this")
    else:
        lines.append(f"  verdict   : REJECTED ({report['verdict_reason']})")
        lines.append("              the hook would fall back to its own prompt")
    return "\n".join(lines)


# --- the probe ------------------------------------------------------------------
async def probe(payload: dict, *, servers: str, timeout: float, allowlist: dict) -> tuple[int, dict]:
    """Send the request, wait, verify. Returns ``(exit_code, report)``."""
    nonce = base64.b64encode(secrets.token_bytes(32)).decode("ascii")
    request = hook.build_request(payload, nonce=nonce, ts=int(time.time()))
    subject = f"approvals.{payload['session_id']}"

    print(f"-> {subject}  ({payload['tool_name']})", file=sys.stderr)
    print(f"   waiting up to {timeout:.0f}s for a responder", file=sys.stderr)

    async with bus.connect(servers, name=bus.client_name("test-request", payload["session_id"])) as b:
        reply = await b.request(subject, request, timeout=timeout)

    trusted, reason = hook.verify_reply(request, reply, allowlist)
    report = describe_reply(reply, trusted=trusted, reason=reason)
    return (EXIT_OK if trusted else EXIT_UNTRUSTED), report


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="test_request.py", description=__doc__)
    parser.add_argument("--tool", default=DEFAULT_TOOL, help="tool name (default: %(default)s)")
    parser.add_argument("--command", help=f"Bash command to ask about (default: {DEFAULT_COMMAND!r})")
    parser.add_argument("--input", dest="raw_input", help="full tool_input as JSON; overrides --command")
    parser.add_argument("--session", help="session id (default: a fresh test-request-<hex>)")
    parser.add_argument("--cwd", default=str(Path.cwd()), help="cwd shown to the operator")
    parser.add_argument("--mode", default="default", help="permission_mode shown to the operator")
    parser.add_argument("--timeout", type=float, help="seconds to wait (default: the config's)")
    parser.add_argument("--servers", help="NATS server(s) (default: the config's)")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG), help="handler-config.json to verify against")
    parser.add_argument("--json", action="store_true", help="print the report as one JSON document")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the request that would be sent and stop; nothing touches the bus",
    )
    return parser


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)

    try:
        tool_input = parse_tool_input(command=args.command, raw=args.raw_input)
    except ValueError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return EXIT_ERROR

    if args.dry_run:
        # Shell quoting is the usual reason --input does not arrive as intended
        # (PowerShell and cmd disagree, see the examples in --help). Print what
        # actually got through, without putting a card in front of an operator.
        payload = build_payload(
            tool_name=args.tool,
            tool_input=tool_input,
            session_id=args.session or default_session_id(),
            cwd=args.cwd,
            permission_mode=args.mode,
        )
        request = hook.build_request(payload, nonce="<dry-run>", ts=int(time.time()))
        print(json.dumps(request, ensure_ascii=False, indent=2))
        return EXIT_OK

    # The allowlist is what makes the verdict meaningful; without it every reply
    # looks untrusted, so say that plainly rather than printing a confusing
    # REJECTED for a perfectly good signature.
    try:
        cfg = configlib.Config.load(args.config, default={})
    except configlib.ConfigError as e:
        print(f"FAIL: {args.config}: {e}", file=sys.stderr)
        return EXIT_ERROR
    # Config iterates keys but exposes no keys(), so dict(cfg) would try to read
    # it as key/value pairs and blow up.
    data = {key: cfg[key] for key in cfg}
    allowlist = hook.allowlist_from_config(data)
    if not allowlist:
        print(f"warning: {args.config} has no registered clients - any reply will be REJECTED",
              file=sys.stderr)

    payload = build_payload(
        tool_name=args.tool,
        tool_input=tool_input,
        session_id=args.session or default_session_id(),
        cwd=args.cwd,
        permission_mode=args.mode,
    )

    servers = args.servers or hook.servers_from_config(data)
    timeout = args.timeout if args.timeout is not None else hook.timeout_from_config(data)

    try:
        code, report = asyncio.run(probe(payload, servers=servers, timeout=timeout, allowlist=allowlist))
    except bus.RequestTimeout:
        print("no answer: nobody decided in time (the hook would fall back to its own prompt)",
              file=sys.stderr)
        return EXIT_ERROR
    except bus.NoResponders:
        print("no answer: no responder is subscribed to approvals.* - is one running?",
              file=sys.stderr)
        return EXIT_ERROR
    except bus.BusError as e:
        print(f"FAIL: NATS: {e}", file=sys.stderr)
        return EXIT_ERROR
    except KeyboardInterrupt:
        return EXIT_ERROR

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(format_report(report))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
