@echo off
setlocal enableextensions

REM ===========================================================================
REM  The device tier of CLAUDE.md 10.11 - the ESP32 responder on the desk.
REM
REM    a request hook.py would send  ->  a card on the glass  ->  one press
REM    ->  a signature the chip made  ->  hook.verify_reply says TRUSTED
REM
REM  This is the acceptance test for the whole of approver-esp32: it is the only
REM  place where the key actually bound to that chip meets the allowlist Claude
REM  Code verifies against. Everything else in the repository can be green while
REM  the object on the desk is useless.
REM
REM  It asks for TWO interactions and says so on screen:
REM    1. press ALLOW (the BOOT button) on the card that appears;
REM    2. press NOTHING for about 20s, which is section 10.10's fail-safe -
REM       no press must mean no reply at all.
REM
REM  Before it can pass:
REM    * NATS is up (section 3) and the device is connected - `nats` on its console;
REM    * the device is REGISTERED, so approver-esp32 is in handler-config.json.
REM      This script does not register it: a token is one-time, and a test that
REM      spent one per run would need a fresh one per run (section 6, 10.7);
REM    * nothing else is on approvals.* - no `responder.py serve`, no browser tab.
REM      The queue group hands each request to exactly ONE responder, so another
REM      one running is a suite that passes for the wrong device. The test
REM      asserts key_id for exactly that reason, but stopping them first saves a
REM      confusing failure.
REM
REM  Exit code: 0 = PASS, 1 = FAIL or skipped.
REM ===========================================================================

set "ROOT=%~dp0.."
set "PYTHON=%ROOT%\.venv\Scripts\python.exe"

REM The venv rather than the `py` launcher: this reaches lib\crypto.py and
REM lib\bus.py, whose dependencies (cryptography, nats-py) are installed by
REM `uv sync` into .venv and are not in the launcher's interpreter.
if not exist "%PYTHON%" (
    echo Could not find %PYTHON%
    echo Run `uv sync` first - README.md has the setup.
    exit /b 1
)

echo.
echo  The board must be powered on, connected to NATS and registered.
echo  Stop any other responder first, or it may answer instead of the device.
echo.

set "AI_REMOTE_ESP32_DEVICE=1"

REM -s is not optional: without it pytest swallows the "press ALLOW now" prompt
REM and a run that is waiting for a finger looks like a run that has hung.
"%PYTHON%" -m pytest "%ROOT%\tests\test_esp32_device.py" -v -s
if errorlevel 1 goto :fail

echo.
echo ==== ESP32 DEVICE TIER PASSED ====
exit /b 0

:fail
echo.
echo ==== ESP32 DEVICE TIER FAILED ====
exit /b 1
