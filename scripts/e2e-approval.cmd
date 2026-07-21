@echo off
setlocal enableextensions enabledelayedexpansion

REM ===========================================================================
REM  End-to-end smoke test of the approval loop (CLAUDE.md 7).
REM
REM    register a responder  ->  responder serve (auto-allow)  ->  hook.py with a
REM    PermissionRequest  ->  verify the signed decision is `allow`.
REM
REM  Runs the real responder.py and hook.py as separate processes over NATS; the
REM  operator's "allow" is fed from a redirected answers file instead of typed.
REM  Requires: NATS running on localhost (CLAUDE.md 3) and the `py` launcher (5).
REM  Uses throwaway config files in %TEMP%; leaves the repo untouched.
REM  Exit code: 0 = PASS, 1 = FAIL.
REM ===========================================================================

set "ROOT=%~dp0.."
set "APPROVER=%ROOT%\approver"
set "KEYID=approver-e2e-approval"
set "SFX=%RANDOM%%RANDOM%"
set "HCFG=%TEMP%\airemote-appr-handler-%SFX%.json"
set "RCFG=%TEMP%\airemote-appr-responder-%SFX%.json"
set "ANS=%TEMP%\airemote-appr-answers-%SFX%.txt"
set "PAYLOAD=%TEMP%\airemote-appr-payload-%SFX%.json"
set "OUT=%TEMP%\airemote-appr-out-%SFX%.json"
set "RUNRESP=%TEMP%\airemote-appr-runresp-%SFX%.cmd"
set "TITLE_H=airemote-appr-h-%SFX%"
set "TITLE_R=airemote-appr-r-%SFX%"
set "RC=1"

echo [1/5] minting one-time token and registering a responder ...
set "TOKEN="
for /f "usebackq delims=" %%i in (`py "%APPROVER%\registration_handler.py" --get-token %KEYID% --config "%HCFG%" 2^>nul`) do set "TOKEN=%%i"
if not defined TOKEN (
    echo    FAIL: could not mint token ^(is the `py` launcher available?^)
    goto :cleanup
)
start "%TITLE_H%" /min py "%APPROVER%\registration_handler.py" --config "%HCFG%" --once
set "REGOK="
for /l %%n in (1,1,15) do (
    if not defined REGOK (
        py "%APPROVER%\responder.py" register "!TOKEN!" --config "%RCFG%" --timeout 3 >nul 2>&1
        if not errorlevel 1 (
            set "REGOK=1"
        ) else (
            ping -n 2 127.0.0.1 >nul 2>&1
        )
    )
)
if not defined REGOK (
    echo    FAIL: responder could not register ^(is NATS running on localhost?^)
    goto :cleanup
)
echo    registered key_id=%KEYID%.

echo [2/5] writing the operator answer ^(allow^) and the request payload ...
> "%ANS%" echo a
>> "%ANS%" echo smoke-approved
py -c "import json,sys; json.dump({'hook_event_name':'PermissionRequest','session_id':'e2e-smoke','tool_name':'Bash','tool_input':{'command':'echo hello'},'permission_mode':'default','cwd':'.'}, open(sys.argv[1],'w',encoding='utf-8'))" "%PAYLOAD%"

echo [3/5] starting responder serve ^(auto-allows the next request^) ...
> "%RUNRESP%" echo @echo off
>> "%RUNRESP%" echo py "%APPROVER%\responder.py" serve --config "%RCFG%" ^< "%ANS%"
start "%TITLE_R%" /min "%RUNRESP%"

echo [4/5] sending PermissionRequest to hook.py ^(retry until responder is ready^) ...
set "GOTOUT="
for /l %%n in (1,1,20) do (
    if not defined GOTOUT (
        py "%APPROVER%\hook.py" --config "%HCFG%" < "%PAYLOAD%" > "%OUT%" 2>nul
        if not errorlevel 1 (
            set "GOTOUT=1"
        ) else (
            ping -n 2 127.0.0.1 >nul 2>&1
        )
    )
)
if not defined GOTOUT (
    echo    FAIL: hook never received a signed reply
    goto :cleanup
)

echo [5/5] verifying the signed decision is allow ...
py -c "import json,sys; d=json.load(open(sys.argv[1],encoding='utf-8')); dec=d['hookSpecificOutput']['decision']; print('    behavior :', dec.get('behavior')); sys.exit(0 if dec.get('behavior')=='allow' else 1)" "%OUT%"
if errorlevel 1 (
    echo    FAIL: decision was not a verified allow
    goto :cleanup
)
set "RC=0"
echo.
echo ==== E2E APPROVAL PASSED ====

:cleanup
REM Reap the background responder (serve runs forever) and any lingering handler.
taskkill /FI "WINDOWTITLE eq %TITLE_R%*" /T /F >nul 2>&1
taskkill /FI "WINDOWTITLE eq %TITLE_H%*" /T /F >nul 2>&1
del "%HCFG%" "%RCFG%" "%ANS%" "%PAYLOAD%" "%OUT%" "%RUNRESP%" >nul 2>&1
if not "%RC%"=="0" (
    echo.
    echo ==== E2E APPROVAL FAILED ====
)
exit /b %RC%
