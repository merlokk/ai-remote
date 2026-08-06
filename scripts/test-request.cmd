@echo off
setlocal enableextensions enabledelayedexpansion

REM ===========================================================================
REM  Send ONE permission request to whatever responder is listening, and print
REM  the answer (CLAUDE.md 7).
REM
REM  A responder-side probe: it sends exactly what hook.py would send, then
REM  judges the reply with hook.verify_reply against the same
REM  approver\handler-config.json allowlist. So the verdict printed here is the
REM  verdict Claude Code would act on -- not an approximation of it.
REM
REM  Use it to exercise a responder without a live Claude Code session: the web
REM  UI, responder.py serve, or responder_yubikey.py serve all look the same
REM  from this side. It is also the quickest way to make the web page's alert
REM  fire on demand.
REM
REM  This file is a launcher: the work is in tools\test_request.py, because cmd
REM  cannot speak NATS and `python -c "..."` mangles quotes here (see the note
REM  in scripts\yubikey-arkg.cmd).
REM
REM  Usage: scripts\test-request.cmd [--command TEXT] [--tool NAME] [--input JSON]
REM             [--session ID] [--timeout SEC] [--config FILE] [--json] [--help]
REM    --command TEXT   the Bash command to ask about (the common case)
REM    --tool NAME      tool name (default: Bash)
REM    --input JSON     full tool_input as a JSON object; overrides --command
REM    --session ID     session id (default: a fresh test-request-<hex>)
REM    --timeout SEC    how long to wait for a decision (default: the config's)
REM    --config FILE    handler config to verify against (default: approver\handler-config.json)
REM    --json           print one JSON document instead of a report
REM
REM  Every option is passed straight through to tools\test_request.py, so its
REM  --help is the authoritative list.
REM
REM  Requires NATS on localhost (CLAUDE.md 3) and a responder that is running.
REM  No elevation needed -- this touches no FIDO device.
REM
REM  Exit codes: 0 = a trusted reply arrived, 1 = error / nobody answered,
REM  3 = a reply arrived but the hook would reject it (usually: the responder
REM  is not registered, or its key is not in this config's allowlist).
REM ===========================================================================

set "ROOT=%~dp0.."
set "TOOL=%ROOT%\tools\test_request.py"

if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-h" goto :usage

REM Prefer the project venv: `py` picks it only when VIRTUAL_ENV is set, and this
REM script is meant to work from any console.
set "PYEXE=%ROOT%\.venv\Scripts\python.exe"
if not exist "%PYEXE%" echo warning: %ROOT%\.venv not found, falling back to the `py` launcher
if not exist "%PYEXE%" set "PYEXE=py"

if not exist "%TOOL%" goto :no_tool

"%PYEXE%" "%TOOL%" %*
set "RC=%ERRORLEVEL%"
if "%RC%"=="1" goto :hint_no_answer
if "%RC%"=="3" goto :hint_untrusted
exit /b %RC%

:hint_no_answer
echo.
echo hint: is a responder running, and is NATS up? (cd nats ^&^& docker compose up -d)
exit /b 1

:hint_untrusted
echo.
echo hint: a responder answered, but its key is not trusted by this config.
echo       Register it, or point --config at the handler config holding its key.
exit /b 3

:no_tool
echo FAIL: %TOOL% not found.
exit /b 1

:usage
echo.
echo Usage: scripts\test-request.cmd [--command TEXT] [--tool NAME] [--input JSON]
echo            [--session ID] [--timeout SEC] [--config FILE] [--json]
echo.
echo Sends one permission request and prints the reply plus the hook's verdict.
echo All options are forwarded to tools\test_request.py --help.
echo.
echo Examples:
echo    scripts\test-request.cmd
echo    scripts\test-request.cmd --command "rm -rf build"
echo    scripts\test-request.cmd --dry-run --command "rm -rf build"
echo.
echo --input quoting is OPPOSITE in the two shells (--dry-run shows what arrived):
echo    cmd.exe     --input "{\"file_path\": \"x.txt\"}"
echo    PowerShell  --input '{"file_path": "x.txt"}'
echo.
exit /b 1
