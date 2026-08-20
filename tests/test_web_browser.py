"""The browser tier of ``approver-web`` — a real browser, a real key, a real reply.

**The acceptance test for the web responder**, and the one thing its own
``npm test`` cannot be: that suite checks byte strings (canonical JSON, the
signing bytes, the two watch-only documents) and has never seen a key. The key
that matters here exists in no file — it is a non-extractable WebCrypto
``CryptoKey`` inside a browser profile — so the only way to test it is to run a
browser, register it, and let ``hook.verify_reply`` judge what it signs.

Three claims, one per section below. All three had been verified by hand and none
of them committed, which is `approver-web/CLAUDE.md` "What is still missing" 1:

1. **a browser signature the hook trusts.** A §7 request goes on the bus, the
   page shows it, Allow is clicked, and the reply verifies against an allowlist
   built the way `hook.py` builds its own — plus every tamper on that reply,
   which comes free once one real signature exists;
2. **the key survives closing the browser.** The browser is *closed*, relaunched
   against the same profile, and signs a second request with the same key — no
   re-registration, and the private half still refuses to be exported. That is
   the custody claim in `browser-key.ts`, and IndexedDB is the whole reason it
   holds;
3. **two browsers, two key_ids.** A second profile registers with its own token,
   both stay registered, and each answers under its own name. The `clients` map
   in `config.json` exists for exactly this.

**Opt-in** (``AI_REMOTE_WEB_BROWSER=1``) for the cost, not for a finger: a
Chrome, a Next dev server and about two minutes. Nothing here waits for a human —
`agent-browser` does the clicking — which is what makes this tier self-checking
where the YubiKey and ESP32 ones are not.

**It cannot be answered for, and it cannot steal a live request.** The app under
test is pointed at its own subject and its own queue group, so a browser tab
somebody left open on ``approvals.*`` neither answers these requests nor loses
its own. That is the ESP32 tier's third precondition designed out instead of
written down.

What it needs:

* NATS up (CLAUDE.md §3) — the real bus, and a real registration handler started
  here against a throwaway config;
* ``agent-browser`` on PATH with a browser installed (``agent-browser doctor``);
* ``approver-web/node_modules`` — ``approver-web\\run.cmd --install`` once.

Run it:

    scripts\\web-approval.cmd

or by hand, with ``-s`` if you want to watch the two minutes go by:

    $env:AI_REMOTE_WEB_BROWSER="1"; .venv\\Scripts\\python.exe -m pytest tests/test_web_browser.py -v -s
"""
from __future__ import annotations

import base64
import copy
import json
import os
import secrets
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

from approver import hook
from lib import bus
from lib import config as configlib
from tests.conftest import requires_nats, requires_web_browser, run_async

REPO = Path(__file__).resolve().parent.parent
WEB = REPO / "approver-web"

#: Two names, because the third claim is that they coexist. Neither is
#: `approver-web`: the live allowlist is not touched here, and a run that rotated
#: the browser somebody is actually using would be a rude test.
KEY_ID_A = "approver-web-tier-a"
KEY_ID_B = "approver-web-tier-b"

#: Its own subject and its own queue group. §6 hands a request to exactly one
#: responder in a group, so sharing `approvals.*` would mean this suite answering
#: for -- or being answered by -- whatever else is subscribed.
SUBJECT = "approvals-webtier.*"
SUBJECT_PREFIX = "approvals-webtier"
QUEUE = "approvers-webtier"

#: How long the app keeps a card. Longer than every wait below, so nothing
#: expires while the browser is being driven.
REQUEST_TTL = 300

#: The dev server compiles on the first request; Turbopack is quick but the first
#: `npm run dev` on a cold `.next` is not instant.
SERVER_READY_TIMEOUT = 240.0
#: Launching Chrome against a brand-new profile directory is the slow step.
BROWSER_TIMEOUT = 180.0
#: One `agent-browser` command against an already-running session.
COMMAND_TIMEOUT = 90.0
#: Long enough to drive a card, short enough that a hang is a failure.
DECISION_TIMEOUT = 120.0
#: And this one only has to outlast nobody clicking anything.
SILENCE_TIMEOUT = 12.0


def _say(text: str) -> None:
    """Progress on stderr, so ``-s`` shows it while the two minutes pass."""
    print(f"    >>> {text}", file=sys.stderr, flush=True)


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def _need(tool: str, how: str) -> str:
    """Fail rather than skip: this tier was asked for by an env var."""
    found = shutil.which(tool)
    if not found:
        pytest.fail(f"{tool} is not on PATH — {how}")
    return found


def _run(argv: list[str], *, timeout: float, tolerate: bool = False) -> str:
    """One short-lived subprocess, returning stdout.

    ``tolerate`` turns a timeout or a non-zero exit into an empty string, which is
    what polling wants: "not yet" is not a failure until the deadline is.
    """
    try:
        done = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            timeout=timeout,
            encoding="utf-8",
            errors="replace",
        )
    except subprocess.TimeoutExpired:
        if tolerate:
            return ""
        pytest.fail(f"timed out after {timeout:.0f}s: {' '.join(argv)}")
    if done.returncode != 0:
        if tolerate:
            return ""
        pytest.fail(
            f"exit {done.returncode}: {' '.join(argv)}\n"
            f"stdout: {done.stdout.strip()}\nstderr: {done.stderr.strip()}"
        )
    return done.stdout


# --- the browser ----------------------------------------------------------------
#: Injected before the page's own scripts, so `eval` can call these by name.
#:
#: Everything the test wants to know about the page is asked through one of these
#: four, and every call site is then a token with no spaces and no quotes in it.
#: The alternative is passing JS — and page text with spaces in it — through a
#: Windows command line, which is a quoting problem nobody should have twice.
PROBE_JS = """
window.__seen = (needle) => !!document.body && document.body.innerText.includes(needle);

/**
 * Hydrated *and* talking to the server. Both halves matter: the heading is
 * server-rendered, so it is on screen long before React has attached anything,
 * and the status bar says "connecting" until the first SSE snapshot arrives.
 */
window.__ready = () =>
  window.__seen("approver-web") &&
  !window.__seen("connecting to the responder") &&
  !window.__seen("checking this browser");

/** How many buttons carry exactly this label — so "one card is up" is a number. */
window.__buttons = (label) =>
  Array.from(document.querySelectorAll("button")).filter(
    (b) => b.innerText.trim() === label,
  ).length;

/** The record `browser-key.ts` put in IndexedDB, plus what the key refuses. */
window.__key = async function () {
  const db = await new Promise((res, rej) => {
    const r = indexedDB.open("approver-web", 1);
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  });
  if (!db.objectStoreNames.contains("keys")) return { found: false };
  const rec = await new Promise((res, rej) => {
    const r = db.transaction("keys", "readonly").objectStore("keys").get("responder");
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  });
  if (!rec) return { found: false };
  // The custody claim asked of the key itself rather than of the flag beside it:
  // a non-extractable private key must refuse both export encodings.
  const refused = [];
  for (const format of ["pkcs8", "jwk"]) {
    try {
      await crypto.subtle.exportKey(format, rec.privateKey);
    } catch (e) {
      refused.push(format);
    }
  }
  return {
    found: true,
    key_id: rec.key_id,
    public_key: rec.public_key,
    extractable: rec.privateKey.extractable,
    refused_exports: refused,
  };
};
"""


class Browser:
    """One `agent-browser` session with a persistent profile directory.

    The profile is the point: it is where Chrome keeps the key store, so closing
    and relaunching against the same directory is what "the key survives closing
    the browser" means.

    **The launching command does not return** — it hosts the daemon for as long as
    the browser lives, and only exits once the session is closed. So it is started
    and left running, and readiness is decided by asking `session list` instead of
    by waiting for an exit. Every command after that one is a short, ordinary
    subprocess. A private `--namespace` keeps all of it away from whatever
    `agent-browser` sessions the machine already has.
    """

    def __init__(self, exe: str, namespace: str, name: str, profile: Path, probe: Path):
        self.exe = exe
        self.namespace = namespace
        self.name = name
        self.profile = profile
        self.probe = probe
        self.launcher: subprocess.Popen | None = None

    def _cmd(self, *args: str) -> list[str]:
        return [self.exe, "--namespace", self.namespace, "--session", self.name, *args]

    # -- launching and navigating
    def open(self, url: str) -> None:
        """Launch (or relaunch) this profile and put it on ``url``."""
        self.launcher = subprocess.Popen(
            [
                self.exe,
                "--namespace", self.namespace,
                "--session", self.name,
                "--profile", str(self.profile),
                "--init-script", str(self.probe),
                "open", "about:blank",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self._wait_until_session_is_up()
        self._navigate(url)
        self.wait_until("__ready()", what=f"the page at {url}")

    def _wait_until_session_is_up(self) -> None:
        deadline = time.monotonic() + BROWSER_TIMEOUT
        listing = [self.exe, "--namespace", self.namespace, "session", "list"]
        while time.monotonic() < deadline:
            if self.launcher and self.launcher.poll() is not None:
                pytest.fail(f"the browser launcher exited with {self.launcher.returncode}")
            # `session list` only asks the daemon what it has; unlike every other
            # command it never launches a browser, so it is safe to poll with.
            if self.name in _run(listing, timeout=30, tolerate=True):
                return
            time.sleep(1.0)
        pytest.fail(f"the browser session {self.name} never came up")

    def _navigate(self, url: str) -> None:
        """Ask the page to go to ``url``, and keep asking until it is there.

        Navigating with `eval` rather than a second `open`: `open` waits for the
        network to go idle, and this page holds an SSE stream open for as long as
        it is on screen, so that moment never comes.

        The loop is not belt-and-braces. A session appears in `session list`
        before its first page has finished being pointed at `about:blank`, so a
        navigation issued in that window is quietly overwritten by the launch
        that is still in flight — which shows up as a browser sitting on
        `about:blank` while every wait below times out on a page that is not
        there. Re-asking is cheaper than trying to detect the moment.
        """
        deadline = time.monotonic() + BROWSER_TIMEOUT
        while time.monotonic() < deadline:
            current = _run(self._cmd("get", "url"), timeout=30, tolerate=True).strip()
            if current.startswith(url.rstrip("/")):
                return
            if current:  # a page exists to ask; otherwise the launch is still coming
                _run(
                    self._cmd("eval", f"location.href='{url}'"),
                    timeout=COMMAND_TIMEOUT,
                    tolerate=True,
                )
            time.sleep(1.5)
        pytest.fail(f"the browser would not navigate to {url}")

    def close(self) -> None:
        if self.launcher is None:
            return
        _run(self._cmd("close"), timeout=COMMAND_TIMEOUT, tolerate=True)
        try:
            self.launcher.wait(timeout=30)
        except subprocess.TimeoutExpired:
            # It hosts the daemon for this namespace, which is ours alone.
            subprocess.run(
                ["taskkill", "/PID", str(self.launcher.pid), "/T", "/F"],
                capture_output=True,
                check=False,
            )
        self.launcher = None

    # -- asking the page things
    def eval(self, expression: str, *, tolerate: bool = False):
        out = _run(self._cmd("eval", expression), timeout=COMMAND_TIMEOUT, tolerate=tolerate).strip()
        if not out:
            return None
        try:
            return json.loads(out)
        except json.JSONDecodeError:
            return out

    def wait_until(self, expression: str, *, what: str, timeout: float = BROWSER_TIMEOUT) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.eval(expression, tolerate=True) is True:
                return
            time.sleep(1.0)
        pytest.fail(f"{what} never appeared ({expression} stayed false for {timeout:.0f}s)")

    def stored_key(self) -> dict:
        record = self.eval("__key()")
        assert isinstance(record, dict), f"the key probe returned {record!r}"
        return record

    # -- and doing things to it
    def fill(self, selector: str, text: str) -> None:
        _run(self._cmd("fill", selector, text), timeout=COMMAND_TIMEOUT)

    def click_button(self, label: str) -> None:
        _run(
            self._cmd("find", "role", "button", "click", "--name", label, "--exact"),
            timeout=COMMAND_TIMEOUT,
        )

    def register(self, token: str, key_id: str) -> None:
        """Register through the page, the way an operator does (§6).

        Deliberately not a `POST /api/register`: the pair has to be generated by
        `browser-key.ts` inside *this profile*, and the form is what does that.
        """
        self.fill("input[name=token]", token)
        self.click_button("Register")
        # The key_id appears on the page only once a key is held under it.
        self.wait_until(f"__seen('{key_id}')", what=f"the registration of {key_id}")

    def answer(self, behavior: str) -> None:
        """Click Allow or Deny on the one card that is up.

        Waiting for *exactly one* is not fussiness: two cards mean two identically
        labelled buttons, and clicking "the Allow button" would then be ambiguous
        rather than wrong-but-deterministic.
        """
        label = behavior.capitalize()
        self.wait_until(f"__buttons('{label}')===1", what=f"one card with a {label} button")
        self.click_button(label)


# --- the app under test ---------------------------------------------------------
class WebApp:
    """The Next dev server, its throwaway config, and a real handler behind it."""

    def __init__(self, root: Path):
        self.root = root
        self.handler_config = root / "handler-config.json"
        self.web_config = root / "web-config.json"
        self.port = _free_port()
        # `localhost` because that is the spelling `run.cmd` prints, so this tier
        # drives what an operator drives. Either works: `allowedDevOrigins` in
        # `next.config.ts` lists both, and without it the numeric one arrives
        # server-rendered, never hydrates, and every wait below times out on a
        # page that looks perfectly fine.
        self.url = f"http://localhost:{self.port}/"
        self.handler: subprocess.Popen | None = None
        self.server: subprocess.Popen | None = None
        self.log_path = root / "server.log"
        self._log = self.log_path.open("w", encoding="utf-8")

    # -- configs
    def token(self, key_id: str) -> str:
        out = _run(
            [
                sys.executable,
                str(REPO / "approver" / "registration_handler.py"),
                "--get-token", key_id,
                "--config", str(self.handler_config),
                # Long enough that the second browser's token is still good when
                # it registers, a minute or so into the run.
                "--ttl", "1800",
            ],
            timeout=60,
        )
        token = out.strip().splitlines()[0].strip()
        assert token.startswith(f"{key_id}."), f"unexpected token {token!r}"
        return token

    def write_config(self) -> None:
        self.web_config.write_text(
            json.dumps(
                {
                    "v": 1,
                    "servers": bus.DEFAULT_SERVERS,
                    "subject": SUBJECT,
                    "queue": QUEUE,
                    "clients": {},
                    "request_ttl": REQUEST_TTL,
                    # Named apart from the live ones too: the app subscribes to
                    # both, and there is nothing on these for it to read.
                    "status_subject": "status-webtier",
                    "activity_subject": "activity-webtier",
                },
                indent=2,
            ),
            encoding="utf-8",
        )

    def clients(self) -> dict:
        """The registered browsers as the app persisted them."""
        return json.loads(self.web_config.read_text(encoding="utf-8")).get("clients", {})

    def allowlist(self) -> dict:
        """Built by `hook.allowlist_from_config`, from the handler's own file."""
        cfg = configlib.Config.load(self.handler_config, default={})
        return hook.allowlist_from_config({key: cfg[key] for key in cfg})

    # -- processes
    def start(self) -> None:
        npm = _need("npm", "install node, or start this from a shell that has it")
        if not (WEB / "node_modules").exists():
            pytest.fail(f"{WEB / 'node_modules'} is missing — run approver-web\\run.cmd --install")

        self.handler = subprocess.Popen(
            [
                sys.executable,
                str(REPO / "approver" / "registration_handler.py"),
                "--config", str(self.handler_config),
            ],
            stdout=self._log,
            stderr=subprocess.STDOUT,
        )
        self.server = subprocess.Popen(
            [npm, "run", "dev", "--", "--port", str(self.port)],
            cwd=str(WEB),
            env={**os.environ, "AI_REMOTE_WEB_CONFIG": str(self.web_config)},
            stdout=self._log,
            stderr=subprocess.STDOUT,
        )
        self._wait_until_serving()

    def _wait_until_serving(self) -> None:
        deadline = time.monotonic() + SERVER_READY_TIMEOUT
        while time.monotonic() < deadline:
            if self.server and self.server.poll() is not None:
                pytest.fail(
                    f"the dev server exited with {self.server.returncode}; see {self.log_path}"
                )
            try:
                with urllib.request.urlopen(self.url, timeout=10) as response:
                    if response.status == 200:
                        return
            except (urllib.error.URLError, TimeoutError, OSError):
                pass
            time.sleep(1.0)
        pytest.fail(f"the dev server never served {self.url}; see {self.log_path}")

    def stop(self) -> None:
        for process in (self.server, self.handler):
            if process is None or process.poll() is not None:
                continue
            # `npm run dev` spawns its own children; kill the tree or the port
            # stays taken and the next run picks a different one for no reason.
            subprocess.run(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                capture_output=True,
                check=False,
            )
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
        self._log.close()


def _build_request(command: str) -> dict:
    payload = {
        "hook_event_name": hook.HOOK_EVENT,
        "session_id": "web-tier-" + secrets.token_hex(4),
        "tool_name": "Bash",
        "tool_input": {"command": command},
        "permission_mode": "default",
        "cwd": str(REPO),
    }
    nonce = base64.b64encode(secrets.token_bytes(32)).decode("ascii")
    return hook.build_request(payload, nonce=nonce, ts=int(time.time()))


def _ask(browser: Browser, command: str, *, behavior: str = "allow"):
    """One §7 exchange, answered in the browser. Returns ``(request, reply)``.

    The request has to be *pending* before anything can be clicked, so it goes out
    on a thread while this one drives the browser — which is the real shape of it:
    `hook.py` is blocked on the bus for as long as a human is reading the card.
    """
    request = _build_request(command)
    box: dict = {}

    def send():
        async def go():
            async with bus.connect(bus.DEFAULT_SERVERS) as b:
                return await b.request(
                    f"{SUBJECT_PREFIX}.{request['session_id']}", request, timeout=DECISION_TIMEOUT
                )

        try:
            box["reply"] = run_async(go())
        except Exception as e:  # noqa: BLE001 - reported below, not swallowed
            box["error"] = e

    sender = threading.Thread(target=send, daemon=True)
    sender.start()
    browser.answer(behavior)
    sender.join(timeout=DECISION_TIMEOUT + 10)

    if "error" in box:
        pytest.fail(f"a button was clicked but nothing usable came back: {box['error']!r}")
    assert "reply" in box, "the request thread never finished"
    return request, box["reply"]


# --- fixtures -------------------------------------------------------------------
@pytest.fixture(scope="module")
def app(tmp_path_factory):
    application = WebApp(tmp_path_factory.mktemp("web-tier"))
    application.write_config()
    _say(f"starting a registration handler and a dev server on port {application.port}")
    application.start()
    _say("the app is serving")
    try:
        yield application
    finally:
        application.stop()


@pytest.fixture(scope="module")
def browser_factory(tmp_path_factory):
    """Browsers with a private `agent-browser` namespace, all closed at the end."""
    exe = _need("agent-browser", "npm i -g agent-browser, then `agent-browser install`")
    namespace = "ai-remote-web-" + secrets.token_hex(3)
    probe = tmp_path_factory.mktemp("web-tier-probe") / "probe.js"
    probe.write_text(PROBE_JS, encoding="utf-8")
    made: list[Browser] = []

    def make(label: str) -> Browser:
        browser = Browser(
            exe, namespace, f"{label}-{secrets.token_hex(3)}",
            tmp_path_factory.mktemp(f"profile-{label}"), probe,
        )
        made.append(browser)
        return browser

    try:
        yield make
    finally:
        for browser in made:
            browser.close()


@pytest.fixture(scope="module")
def browser_a(app, browser_factory):
    """A browser that has registered as `KEY_ID_A` and holds the key for it."""
    browser = browser_factory("a")
    _say("launching browser A")
    browser.open(app.url)
    browser.register(app.token(KEY_ID_A), KEY_ID_A)
    _say(f"browser A registered as {KEY_ID_A}")
    return browser


@pytest.fixture(scope="module")
def allowed(browser_a):
    """A request browser A answered with Allow, and the reply it signed."""
    _say("asking for a decision and clicking Allow")
    return _ask(browser_a, "echo 'web tier: a browser signature'")


@pytest.fixture(scope="module")
def restarted(app, browser_a, allowed):
    """Browser A closed, relaunched on the same profile, and asked again.

    Returns everything the section-2 tests compare: the stored key record and the
    persisted registration either side of the restart, plus the exchange the
    relaunched browser answered.
    """
    before = browser_a.stored_key()
    registered_before = app.clients().get(KEY_ID_A)
    _say("closing browser A completely")
    browser_a.close()
    _say("relaunching it against the same profile")
    browser_a.open(app.url)
    after = browser_a.stored_key()
    request, reply = _ask(browser_a, "echo 'web tier: the key survived a restart'")
    return {
        "before": before,
        "after": after,
        "registered_before": registered_before,
        "registered_after": app.clients().get(KEY_ID_A),
        "request": request,
        "reply": reply,
    }


@pytest.fixture(scope="module")
def browser_b(app, browser_factory, restarted):
    """A second browser: its own profile, its own token, its own key_id.

    Ordered after `restarted` on purpose — until then browser A is the only one
    registered, which is the state the first two sections are about.
    """
    browser = browser_factory("b")
    _say("launching browser B")
    browser.open(app.url)
    browser.register(app.token(KEY_ID_B), KEY_ID_B)
    _say(f"browser B registered as {KEY_ID_B}")
    return browser


# === 1. a browser signature the hook trusts ====================================
@requires_nats
@requires_web_browser
def test_the_browser_signed_a_reply_the_hook_trusts(app, allowed):
    """**The acceptance criterion**, in the hook's own words.

    `verify_reply` is the function `hook.py` calls, against an allowlist read the
    way `hook.py` reads one — so a pass here is Claude Code acting on a click in
    a browser.
    """
    request, reply = allowed
    trusted, reason = hook.verify_reply(request, reply, app.allowlist())
    assert trusted, f"the hook would reject the browser's reply: {reason}"


@requires_nats
@requires_web_browser
def test_it_answered_as_itself(allowed):
    """Which key_id answered is not a formality: it is what the reply is judged by."""
    _, reply = allowed
    assert reply.get("key_id") == KEY_ID_A


@requires_nats
@requires_web_browser
def test_the_verdict_is_the_button_that_was_clicked(allowed):
    _, reply = allowed
    assert reply.get("behavior") == "allow"
    # Neither optional field was filled in, so neither may appear.
    assert reply.get("reason", "") == ""
    assert "updated_input" not in reply or reply["updated_input"] is None


@requires_nats
@requires_web_browser
def test_the_same_reply_with_the_verdict_flipped_is_rejected(app, allowed):
    """The signature is over the verdict, not merely next to it."""
    request, reply = allowed
    tampered = copy.deepcopy(reply)
    tampered["behavior"] = "deny"
    trusted, reason = hook.verify_reply(request, tampered, app.allowlist())
    assert not trusted, "a flipped verdict was accepted — the signature covers nothing"
    assert "signature" in reason


@requires_nats
@requires_web_browser
def test_every_echoed_field_binds_the_verdict_to_this_request(app, allowed):
    """§7's echo fields, one at a time — each of them a different replay."""
    request, reply = allowed
    allowlist = app.allowlist()
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
        trusted, _ = hook.verify_reply(request, tampered, allowlist)
        assert not trusted, f"a reply with a changed {field} was accepted"


@requires_nats
@requires_web_browser
def test_a_reply_that_claims_another_responder_is_rejected(app, allowed):
    """`key_id` selects the key *and* the scheme on the verifying side (§7), so a
    reply that renames itself is checked against somebody else's key and fails."""
    request, reply = allowed
    allowlist = app.allowlist()
    for borrowed in (KEY_ID_B, "no-such-responder"):
        tampered = copy.deepcopy(reply)
        tampered["key_id"] = borrowed
        trusted, _ = hook.verify_reply(request, tampered, allowlist)
        assert not trusted, f"a reply claiming to be {borrowed} was accepted"


@requires_nats
@requires_web_browser
def test_a_reply_with_no_signature_at_all_is_rejected(app, allowed):
    """The failure mode an unregistered browser produces, and the one that must
    never read as "nothing was wrong with it"."""
    request, reply = allowed
    allowlist = app.allowlist()
    for missing in ({}, {"sig": ""}, {"sig": None}, {"sig": "not base64"}):
        tampered = copy.deepcopy(reply)
        tampered.pop("sig", None)
        tampered.update(missing)
        trusted, _ = hook.verify_reply(request, tampered, allowlist)
        assert not trusted


# === 2. the key survives closing the browser ===================================
@requires_nats
@requires_web_browser
def test_the_private_key_cannot_be_exported(restarted):
    """The custody claim, asked of the key rather than of the flag beside it.

    `extractable: false` is what `browser-key.ts` passes to `generateKey`; this is
    the browser refusing both export encodings afterwards, which is the half that
    means the server — which sees every request — cannot forge a decision.
    """
    after = restarted["after"]
    assert after["found"], "the relaunched browser holds no key"
    assert after["extractable"] is False
    assert sorted(after["refused_exports"]) == ["jwk", "pkcs8"]


@requires_nats
@requires_web_browser
def test_the_same_key_is_there_after_a_full_restart(restarted):
    """IndexedDB rather than localStorage is the whole reason: the *handle* is
    stored and the material stays in the browser's key store, so the key comes
    back instead of being regenerated."""
    before, after = restarted["before"], restarted["after"]
    assert after["key_id"] == before["key_id"] == KEY_ID_A
    assert after["public_key"] == before["public_key"]


@requires_nats
@requires_web_browser
def test_the_relaunched_browser_signs_a_reply_the_hook_still_trusts(app, restarted):
    """The only test that says the surviving key still *works*: a key that comes
    back as an object but no longer signs would pass everything above."""
    trusted, reason = hook.verify_reply(
        restarted["request"], restarted["reply"], app.allowlist()
    )
    assert trusted, f"the reply signed after the restart was rejected: {reason}"
    assert restarted["reply"].get("key_id") == KEY_ID_A


@requires_nats
@requires_web_browser
def test_nothing_was_registered_a_second_time(restarted):
    """A relaunch that quietly re-registered would pass both tests above and hide
    what they are for. The persisted entry is the witness: same key, same moment."""
    assert restarted["registered_after"] == restarted["registered_before"]
    assert restarted["registered_after"]["public_key"] == restarted["after"]["public_key"]


# === 3. two browsers, two key_ids ==============================================
@requires_nats
@requires_web_browser
def test_a_second_browser_registers_without_evicting_the_first(app, browser_b):
    """What the `clients` map is for: one key per `key_id`, so a second token adds
    an entry instead of rotating the first one out."""
    clients = app.clients()
    assert sorted(clients) == sorted([KEY_ID_A, KEY_ID_B])
    assert clients[KEY_ID_A]["public_key"] != clients[KEY_ID_B]["public_key"]


@requires_nats
@requires_web_browser
def test_each_browser_holds_its_own_key(browser_a, browser_b):
    a, b = browser_a.stored_key(), browser_b.stored_key()
    assert a["key_id"] == KEY_ID_A and b["key_id"] == KEY_ID_B
    assert a["public_key"] != b["public_key"]


@requires_nats
@requires_web_browser
def test_the_second_browser_answers_under_its_own_name(app, browser_b):
    """And the hook trusts it: a different key, a different allowlist entry, the
    same verification path. This is the whole point of two key_ids."""
    request, reply = _ask(browser_b, "echo 'web tier: the second browser'")
    assert reply.get("key_id") == KEY_ID_B
    trusted, reason = hook.verify_reply(request, reply, app.allowlist())
    assert trusted, f"the second browser's reply was rejected: {reason}"


@requires_nats
@requires_web_browser
def test_the_first_browser_still_answers_too(app, browser_a, browser_b):
    """The one that would fail if registering B had rotated A out on either side
    of the wire — the config here, or the allowlist there."""
    request, reply = _ask(browser_a, "echo 'web tier: and the first one still works'")
    assert reply.get("key_id") == KEY_ID_A
    trusted, reason = hook.verify_reply(request, reply, app.allowlist())
    assert trusted, f"the first browser stopped being trusted: {reason}"


# === and the fail-safe, which is the other half of what this app is for =========
@requires_nats
@requires_web_browser
def test_clicking_nothing_sends_nothing(app, browser_a):
    """**No reply is the safe outcome** — the rule this app inherits.

    A card goes up and nobody touches it. The hook times out and Claude Code asks
    in its own terminal; a reply arriving here would be a page answering on a
    timer, which is a page approving things nobody read.

    Last in the file on purpose: it leaves a card on screen that nothing clears
    until it expires, and a second card is exactly what `Browser.answer` refuses
    to guess between.
    """
    request = _build_request("echo 'web tier: nobody should answer this'")

    async def go():
        async with bus.connect(bus.DEFAULT_SERVERS) as b:
            return await b.request(
                f"{SUBJECT_PREFIX}.{request['session_id']}", request, timeout=SILENCE_TIMEOUT
            )

    with pytest.raises(bus.RequestTimeout):
        run_async(go())
