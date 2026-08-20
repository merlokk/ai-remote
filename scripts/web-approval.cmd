@echo off
setlocal enableextensions

REM ===========================================================================
REM  The browser tier of approver-web - the register -> request -> signed
REM  decision loop, driven end to end with nothing typed by hand.
REM
REM    a token from a real handler  ->  Register clicked in a real browser
REM    ->  a request hook.py would send  ->  Allow clicked  ->  a signature
REM    made by a non-extractable key inside that browser  ->  verify_reply
REM    says trusted
REM
REM  This is the acceptance test for approver-web, and the one thing its own
REM  `npm test` cannot be: that suite checks byte strings and has never seen a
REM  key. The key here exists in no file - it is a WebCrypto CryptoKey inside a
REM  browser profile - so the only way to test it is to run a browser.
REM
REM  It also proves the two things that were verified by hand and never
REM  committed: the key survives CLOSING the browser (closed, relaunched on the
REM  same profile, signs again with the same key), and two browsers can hold two
REM  key_ids at once without rotating each other out.
REM
REM  Self-checking and unattended: agent-browser does the clicking, so unlike
REM  yubikey-approval.cmd and esp32-approval.cmd there is no finger in the loop.
REM
REM  Before it can pass:
REM    * NATS is up (section 3) - `cd nats && docker compose up -d`;
REM    * agent-browser is installed with a browser (`agent-browser doctor`);
REM    * approver-web\node_modules exists - approver-web\run.cmd --install once.
REM
REM  It touches nothing of yours: a throwaway handler config and a throwaway web
REM  config under the pytest temp root, two throwaway browser profiles, its own
REM  subject and its own queue group. So it neither answers a live
REM  PermissionRequest nor loses one to a responder you already have running -
REM  which is the one precondition esp32-approval.cmd has to ask for.
REM
REM  Exit code: 0 = PASS, 1 = FAIL or skipped.
REM ===========================================================================

set "ROOT=%~dp0.."
set "PYTHON=%ROOT%\.venv\Scripts\python.exe"

REM The venv rather than the `py` launcher: this reaches lib\bus.py and
REM lib\crypto.py, whose dependencies (nats-py, cryptography) are installed by
REM `uv sync` into .venv and are not in the launcher's interpreter.
if not exist "%PYTHON%" (
    echo Could not find %PYTHON%
    echo Run `uv sync` first - README.md has the setup.
    exit /b 1
)

where agent-browser >nul 2>&1
if errorlevel 1 (
    echo agent-browser is not on PATH.
    echo   npm i -g agent-browser  ^&^&  agent-browser install
    exit /b 1
)

if not exist "%ROOT%\approver-web\node_modules" (
    echo %ROOT%\approver-web\node_modules is missing.
    echo Run approver-web\run.cmd --install once, then try again.
    exit /b 1
)

echo.
echo  Starting a registration handler, a dev server and two browsers.
echo  Nothing to press - this takes a couple of minutes.
echo.

set "AI_REMOTE_WEB_BROWSER=1"

REM -s so the progress lines show while the two minutes pass; without it a run
REM that is launching Chrome looks like a run that has hung.
"%PYTHON%" -m pytest "%ROOT%\tests\test_web_browser.py" -v -s
if errorlevel 1 goto :fail

echo.
echo ==== WEB BROWSER TIER PASSED ====
exit /b 0

:fail
echo.
echo ==== WEB BROWSER TIER FAILED ====
exit /b 1
