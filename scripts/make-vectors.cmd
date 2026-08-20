@echo off
setlocal enableextensions

REM ===========================================================================
REM  Regenerate the cross-language parity vectors -- tier 2 of
REM  approver-esp32\tests.md section 10.11.
REM
REM  The firmware and approver\protocol.py have to agree on exact byte strings,
REM  and the failure when they stop agreeing is invisible from the device's own
REM  side: the hook rejects every reply and Claude Code keeps asking in its own
REM  terminal, which looks exactly like a responder that is not answering. So
REM  the expectations the host tests compare against are produced by the Python
REM  implementation itself and committed:
REM
REM    approver-esp32\host_test\vectors\parity_vectors.h
REM    approver-esp32\components\crypto\selftest_vector.h
REM
REM  Run this after changing approver\protocol.py or lib\crypto.py, then commit
REM  what it wrote. You do not have to remember to: tests\test_esp32_vectors.py
REM  regenerates into memory on every pytest run and fails when the committed
REM  files are stale. This script is the shorter way to make it green again.
REM
REM  Usage: scripts\make-vectors.cmd [--check]
REM    --check    write nothing; exit 1 if a committed file is stale
REM
REM  This file is a launcher: the work is in approver-esp32\tools\make_vectors.py.
REM
REM  Needs nothing running -- no NATS, no board, no hardware. It does need the
REM  venv, because lib\crypto.py imports `cryptography`.
REM
REM  Exit codes: 0 = written (or already up to date), 1 = --check found something
REM  stale, or the generator failed.
REM ===========================================================================

set "ROOT=%~dp0.."
set "TOOL=%ROOT%\approver-esp32\tools\make_vectors.py"

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
echo Usage: scripts\make-vectors.cmd [--check]
echo.
echo Regenerates the committed parity vectors from approver\protocol.py and
echo lib\crypto.py (tier 2 of approver-esp32\tests.md section 10.11).
echo.
echo    scripts\make-vectors.cmd            REM write them, print what changed
echo    scripts\make-vectors.cmd --check    REM write nothing, exit 1 if stale
echo.
exit /b 1
