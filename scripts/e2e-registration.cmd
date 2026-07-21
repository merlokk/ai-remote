@echo off
setlocal enableextensions enabledelayedexpansion

REM ===========================================================================
REM  End-to-end check for the responder registration flow (CLAUDE.md 6).
REM
REM    mint one-time token  ->  serve handler (--once)  ->  responder register
REM    ->  verify handler allowlist matches the responder's public key.
REM
REM  Requires: NATS running on localhost (CLAUDE.md 3) and the `py` launcher (5).
REM  Uses throwaway config files in %TEMP%; leaves the repo untouched.
REM  Exit code: 0 = PASS, 1 = FAIL.
REM ===========================================================================

set "ROOT=%~dp0.."
set "APPROVER=%ROOT%\approver"
set "KEYID=approver-e2e"
set "HCFG=%TEMP%\airemote-e2e-handler-%RANDOM%%RANDOM%.json"
set "RCFG=%TEMP%\airemote-e2e-responder-%RANDOM%%RANDOM%.json"
set "TITLE=airemote-e2e-%RANDOM%%RANDOM%"
set "RC=1"

echo [1/4] minting one-time token for %KEYID% ...
set "TOKEN="
for /f "usebackq delims=" %%i in (`py "%APPROVER%\registration_handler.py" --get-token %KEYID% --config "%HCFG%" 2^>nul`) do set "TOKEN=%%i"
if not defined TOKEN (
    echo    FAIL: could not mint token ^(is the `py` launcher available?^)
    goto :cleanup
)
echo    token: !TOKEN!

echo [2/4] starting registration handler ^(--once, exits after first success^) ...
start "%TITLE%" /min py "%APPROVER%\registration_handler.py" --config "%HCFG%" --once

echo [3/4] registering responder ^(retry until the handler is subscribed^) ...
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
echo    registered.

echo [4/4] verifying handler allowlist against responder key ...
py -c "import json,sys; k=sys.argv[1]; h=json.load(open(sys.argv[2],encoding='utf-8')); r=json.load(open(sys.argv[3],encoding='utf-8')); c=h.get('clients',{}).get(k); ok=bool(c) and c.get('pubkey')==r.get('public_key') and h.get('pending_tokens')==[]; print('    handler pubkey :', c and c.get('pubkey')); print('    responder pub  :', r.get('public_key')); print('    pending_tokens :', h.get('pending_tokens')); sys.exit(0 if ok else 1)" %KEYID% "%HCFG%" "%RCFG%"
if errorlevel 1 (
    echo    FAIL: allowlist does not match the responder key
    goto :cleanup
)
set "RC=0"
echo.
echo ==== E2E PASSED ====

:cleanup
REM Reap the handler if it is still waiting (e.g. registration never succeeded).
taskkill /FI "WINDOWTITLE eq %TITLE%*" /T /F >nul 2>&1
del "%HCFG%" >nul 2>&1
del "%RCFG%" >nul 2>&1
if not "%RC%"=="0" (
    echo.
    echo ==== E2E FAILED ====
)
exit /b %RC%
