@echo off
setlocal enableextensions

REM ===========================================================================
REM  Regenerate the ARKG parity vectors -- tier 2 of
REM  approver-esp32-yubikey\tests.md section 10.11, and section 10.18's half of
REM  it.
REM
REM  This is the sibling of make-vectors.cmd and it exists for the same reason
REM  in a sharper form. That one pins the bytes a decision is signed over; this
REM  one pins the *derivation of the key that signs it*. The ESP32-S3 responder
REM  derives an ARKG public key on the chip, registers it, and every verdict is
REM  then signed inside a security key that reconstructs the private half from a
REM  key handle. Nothing checks that the two match. If the derivation is wrong by
REM  one byte, the key signs with a scalar nobody registered, hook.py rejects
REM  every reply, and from the desk that looks exactly like a device that is not
REM  answering.
REM
REM  So the expectations are produced by an independent Python implementation of
REM  the draft and committed:
REM
REM    approver-esp32-yubikey\host_test\vectors\arkg_vectors.h
REM    approver-esp32-yubikey\components\arkg\arkg_selftest_vector.h
REM
REM  The second one ships **inside the firmware**: `key selftest` on the console
REM  runs the two steps the host tier cannot (the ECDH and the point addition)
REM  against it, on the chip, with nothing plugged into the OTG port.
REM
REM  Run this after changing components\arkg or the generator, then commit what
REM  it wrote. You do not have to remember to: tests\test_esp32yk_arkg_vectors.py
REM  regenerates into memory on every pytest run and fails when the committed
REM  files are stale. This script is the shorter way to make it green again.
REM
REM  Usage: scripts\esp32yk-make-vectors.cmd [--check]
REM    --check    write nothing; exit 1 if a committed file is stale
REM
REM  This file is a launcher: the work is in
REM  approver-esp32-yubikey\tools\make_arkg_vectors.py.
REM
REM  Needs nothing running -- no NATS, no board, no security key. It does need
REM  the venv, because the generator imports `cryptography`. It does NOT need the
REM  yubikey extra: the generator is deliberately independent of fido2 so a bare
REM  `uv sync` can still regenerate. Where fido2 IS installed, the pytest guard
REM  additionally cross-checks the result against Yubico's own implementation,
REM  which is the agreement that actually matters.
REM
REM  Exit codes: 0 = written (or already up to date), 1 = --check found something
REM  stale, or the generator failed.
REM ===========================================================================

set "ROOT=%~dp0.."
set "TOOL=%ROOT%\approver-esp32-yubikey\tools\make_arkg_vectors.py"

if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-h" goto :usage

REM Prefer the project venv: `py` picks it only when VIRTUAL_ENV is set, and this
REM script is meant to work from any console.
set "PYEXE=%ROOT%\.venv\Scripts\python.exe"
if not exist "%PYEXE%" echo warning: %ROOT%\.venv not found, falling back to the `py` launcher
if not exist "%PYEXE%" set "PYEXE=py"

if not exist "%TOOL%" goto :no_tool

"%PYEXE%" "%TOOL%" %*
exit /b %ERRORLEVEL%

:no_tool
echo FAIL: %TOOL% not found.
exit /b 1

:usage
echo.
echo Usage: scripts\esp32yk-make-vectors.cmd [--check]
echo.
echo Regenerates the committed ARKG vectors for the ESP32-S3 responder
echo (tier 2 of approver-esp32-yubikey\tests.md section 10.11).
echo.
echo    scripts\esp32yk-make-vectors.cmd            REM write them
echo    scripts\esp32yk-make-vectors.cmd --check    REM write nothing, exit 1 if stale
echo.
exit /b 1
