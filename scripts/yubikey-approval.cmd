@echo off
setlocal enableextensions enabledelayedexpansion

REM ===========================================================================
REM  The full approval loop with the key on a YubiKey (CLAUDE.md 8.7 + 6/7).
REM
REM    1. throwaway handler config + a one-time token
REM    2. make-credential          - an ARKG seed key on the device (FIRST TOUCH)
REM    3. register                 - derive + publish the public half (NO device)
REM    4. responder_yubikey serve  - re-derives offline, then waits for requests
REM    5. hook.py <- PermissionRequest - the key signs the decision (SECOND TOUCH)
REM    6. verify hook.py printed a signed allow/deny
REM
REM  Everything here is the real thing: the real registration handler, the real
REM  responder_yubikey.py and the real hook.py as separate processes over NATS.
REM  Only the operator's a/d keystroke is scripted (fed from a redirected answers
REM  file, as in scripts\e2e-approval.cmd); the touch is not, and cannot be.
REM
REM  Why make-credential comes BEFORE register: `register --credential FILE` needs
REM  no device at all, so the touch is spent once, up front. Re-running this script
REM  with --credential <that file> then costs no makeCredential touch at all.
REM
REM  Step 3 also prints what the handler stored in its allowlist -- key_type=p256
REM  and the compressed point -- which is the whole interop claim: hook.py verifies
REM  a YubiKey reply with the same code path as a software one.
REM
REM  Like scripts\yubikey-arkg.cmd (and unlike scripts\e2e-*.cmd) this is not a
REM  fully unattended test: a human finger is in the loop.
REM
REM  ELEVATION: run this from an ALREADY elevated console -- e.g. `sudo cmd`, then
REM  this script. It does not elevate itself. Windows hands FIDO devices to the
REM  WebAuthn API only, so an unelevated process sees no key, and the previewSign
REM  output is dropped. Because elevating drops VIRTUAL_ENV, this script calls the
REM  venv interpreter by path instead of relying on the `py` launcher.
REM
REM  Requires NATS on localhost (CLAUDE.md 3) and `uv sync --extra yubikey`.
REM
REM  NOTE ON STRUCTURE: not one `if` here opens a parenthesised block; every branch
REM  is a single-line `if ... goto :label`. That is deliberate. With LF-only line
REM  endings cmd resumes at the wrong byte offset after a `goto` taken from inside
REM  a block and silently SKIPS whole if-blocks -- an earlier draft of this file
REM  parsed its first option and then jumped straight to the --help test, so
REM  `--credential X --timeout N` died with "unknown argument: --timeout". It is
REM  offset-dependent (the shorter scripts here get away with it), so rather than
REM  depend on the file's line endings surviving every editor, there are no blocks.
REM
REM  Usage: scripts\yubikey-approval.cmd [--ctx LABEL] [--credential FILE]
REM             [--out FILE] [--rp-id ID] [--attest] [--require-yubikey]
REM             [--deny] [--timeout SEC]
REM    --ctx LABEL        derivation context label (default: ai-remote-approvals)
REM    --credential FILE  reuse a saved credential; skips step 2 and its touch
REM    --out FILE         where to keep the new credential (default: in %TEMP%)
REM    --rp-id ID         relying party of the credential (default: example.com)
REM    --attest           report the "is this really a YubiKey?" check at register
REM    --require-yubikey  imply --attest and refuse to register unless it verifies
REM    --deny             answer deny instead of allow, and expect a signed deny
REM    --timeout SEC      how long hook.py waits for the decision (default: 120)
REM
REM  Exit codes: 0 = PASS, 1 = FAIL, 2 = --require-yubikey said this is not a
REM  YubiKey (nothing was registered).
REM ===========================================================================

set "ROOT=%~dp0.."
set "APPROVER=%ROOT%\approver"
set "TOOL=%ROOT%\tools\yubikey_exec.py"
set "RESPONDER=%APPROVER%\responder_yubikey.py"
set "KEYID=approver-yk-e2e"
set "CTX=ai-remote-approvals"
set "RPID=example.com"
set "SFX=%RANDOM%%RANDOM%"
set "CRED=%TEMP%\airemote-ykappr-cred-%SFX%.json"
set "HCFG=%TEMP%\airemote-ykappr-handler-%SFX%.json"
set "RCFG=%TEMP%\airemote-ykappr-responder-%SFX%.json"
set "TOKFILE=%TEMP%\airemote-ykappr-token-%SFX%.txt"
set "ANS=%TEMP%\airemote-ykappr-answers-%SFX%.txt"
set "PAYLOAD=%TEMP%\airemote-ykappr-payload-%SFX%.json"
set "OUT=%TEMP%\airemote-ykappr-out-%SFX%.json"
set "ERR=%TEMP%\airemote-ykappr-err-%SFX%.txt"
set "RUNRESP=%TEMP%\airemote-ykappr-runresp-%SFX%.cmd"
set "TITLE_H=airemote-ykappr-h-%SFX%"
set "TITLE_R=airemote-ykappr-r-%SFX%"
set "ATTEST="
set "ANSWER=a"
set "EXPECT=allow"
set "HTIMEOUT=120"
set "OWNCRED=1"
set "RC=1"

REM Prefer the project venv (elevation loses VIRTUAL_ENV, so `py` would pick the
REM global interpreter, which has neither cryptography nor fido2).
set "PYEXE=%ROOT%\.venv\Scripts\python.exe"
if not exist "%PYEXE%" echo warning: %ROOT%\.venv not found, falling back to the `py` launcher
if not exist "%PYEXE%" set "PYEXE=py"

REM --- argument parsing: one single-line `if` per option, bodies at labels ------
:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="--ctx" goto :opt_ctx
if /i "%~1"=="--credential" goto :opt_credential
if /i "%~1"=="--out" goto :opt_out
if /i "%~1"=="--rp-id" goto :opt_rpid
if /i "%~1"=="--attest" goto :opt_attest
if /i "%~1"=="--require-yubikey" goto :opt_require
if /i "%~1"=="--deny" goto :opt_deny
if /i "%~1"=="--timeout" goto :opt_timeout
if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-h" goto :usage
echo unknown argument: %~1
goto :usage

:opt_ctx
set "CTX=%~2"
shift
shift
goto :parse

:opt_credential
set "CRED=%~2"
set "OWNCRED="
shift
shift
goto :parse

:opt_out
set "CRED=%~2"
shift
shift
goto :parse

:opt_rpid
set "RPID=%~2"
shift
shift
goto :parse

:opt_attest
set "ATTEST=--attest"
shift
goto :parse

:opt_require
set "ATTEST=--require-yubikey"
shift
goto :parse

:opt_deny
set "ANSWER=d"
set "EXPECT=deny"
shift
goto :parse

:opt_timeout
set "HTIMEOUT=%~2"
shift
shift
goto :parse

:parsed
if not exist "%RESPONDER%" goto :fail_no_responder
if not defined OWNCRED if not exist "%CRED%" goto :fail_no_credential

echo ==== YubiKey approval loop ====
echo   interpreter : %PYEXE%
echo   key_id      : %KEYID%
echo   ctx         : %CTX%
echo   rp_id       : %RPID%
echo   credential  : %CRED%
if not defined OWNCRED echo                 ^(reused - no makeCredential touch needed^)
echo   decision    : %EXPECT%
echo   hook wait   : %HTIMEOUT%s
echo.

echo [1/6] writing a throwaway handler config and minting a one-time token ...
REM The timeout has to be in the config: hook.py takes it from there, not from a
REM flag, and the 60s default is tight when a human must notice a blinking key.
"%PYEXE%" -c "import json,sys; json.dump({'v':1,'servers':'nats://127.0.0.1:4222','timeout':int(sys.argv[2]),'pending_tokens':[],'clients':{}}, open(sys.argv[1],'w',encoding='utf-8'))" "%HCFG%" %HTIMEOUT%
if errorlevel 1 goto :fail_python

set "TOKEN="
"%PYEXE%" "%APPROVER%\registration_handler.py" --get-token %KEYID% --config "%HCFG%" > "%TOKFILE%" 2>nul
if exist "%TOKFILE%" for /f "usebackq delims=" %%i in ("%TOKFILE%") do set "TOKEN=%%i"
del "%TOKFILE%" >nul 2>&1
if not defined TOKEN goto :fail_token
echo    token minted for %KEYID% ^(15 min TTL^).
echo.

if defined OWNCRED goto :do_make_credential
echo [2/6] reusing the saved credential, no touch needed ...
goto :have_credential

:do_make_credential
echo [2/6] make-credential -- TOUCH YOUR YUBIKEY WHEN IT BLINKS ...
"%PYEXE%" "%TOOL%" make-credential --out "%CRED%" --rp-id "%RPID%"
if errorlevel 1 goto :fail_make
if not exist "%CRED%" goto :fail_saved

:have_credential
echo.

echo [3/6] starting the registration handler and registering the derived key ...
start "%TITLE_H%" /min "%PYEXE%" "%APPROVER%\registration_handler.py" --config "%HCFG%" --once
REM Give it a moment to connect and subscribe. No retry loop on purpose: register
REM prints an attestation report worth seeing, and a re-run costs no touch now that
REM the credential is on disk.
ping -n 4 127.0.0.1 >nul 2>&1
"%PYEXE%" "%RESPONDER%" register "!TOKEN!" --credential "%CRED%" --ctx "%CTX%" --rp-id "%RPID%" --config "%RCFG%" --timeout 5 %ATTEST%
if errorlevel 2 goto :fail_notyubikey
if errorlevel 1 goto :fail_register
echo.

echo    what the handler put in its allowlist ^(this is what hook.py verifies with^):
"%PYEXE%" -c "import json,sys; c=json.load(open(sys.argv[1],encoding='utf-8'))['clients'][sys.argv[2]]; print('    key_type :', c.get('key_type')); print('    pubkey   :', c.get('pubkey')); sys.exit(0 if c.get('key_type')=='p256' else 1)" "%HCFG%" %KEYID%
if errorlevel 1 goto :fail_allowlist
echo.

echo [4/6] starting responder_yubikey serve ^(re-derives offline, no touch^) ...
> "%ANS%" echo %ANSWER%
>> "%ANS%" echo signed on the yubikey
"%PYEXE%" -c "import json,sys; json.dump({'hook_event_name':'PermissionRequest','session_id':'yk-approval-smoke','tool_name':'Bash','tool_input':{'command':'echo hello from the yubikey'},'permission_mode':'default','cwd':'.'}, open(sys.argv[1],'w',encoding='utf-8'))" "%PAYLOAD%"
if errorlevel 1 goto :fail_python

REM `start` cannot redirect stdin, so the answer file is piped in from a wrapper.
REM The wrapper pauses at the end so a startup failure stays readable in its window.
> "%RUNRESP%" echo @echo off
>> "%RUNRESP%" echo title %TITLE_R%
>> "%RUNRESP%" echo "%PYEXE%" "%RESPONDER%" serve --config "%RCFG%" ^< "%ANS%"
>> "%RUNRESP%" echo echo.
>> "%RUNRESP%" echo echo responder exited with errorlevel %%errorlevel%%. Close this window when done.
>> "%RUNRESP%" echo pause
start "%TITLE_R%" "%RUNRESP%"
ping -n 6 127.0.0.1 >nul 2>&1
echo    responder window titled "%TITLE_R%" is up.
echo.

echo [5/6] sending a PermissionRequest through hook.py ...
echo    the responder will auto-answer "%EXPECT%", then the key needs a signature:
echo    ^>^>^> TOUCH YOUR YUBIKEY WHEN IT BLINKS ^<^<^<
"%PYEXE%" "%APPROVER%\hook.py" --config "%HCFG%" < "%PAYLOAD%" > "%OUT%" 2>"%ERR%"
if errorlevel 1 goto :fail_hook
echo.

echo [6/6] verifying the decision hook.py printed ...
"%PYEXE%" -c "import json,sys; d=json.load(open(sys.argv[1],encoding='utf-8')); dec=d['hookSpecificOutput']['decision']; print('    behavior :', dec.get('behavior')); sys.exit(0 if dec.get('behavior')==sys.argv[2] else 1)" "%OUT%" %EXPECT%
if errorlevel 1 goto :fail_decision

set "RC=0"
echo.
echo ==== YUBIKEY APPROVAL PASSED ====
echo A signature made inside the YubiKey was verified by hook.py against the
echo allowlist entry registered for %KEYID% ^(key_type=p256^).
goto :cleanup

:fail_no_responder
echo FAIL: cannot find %RESPONDER%
exit /b 1

:fail_no_credential
echo FAIL: --credential %CRED% does not exist
exit /b 1

:fail_python
echo.
echo FAIL: could not run %PYEXE%.
echo   - Is the venv there?  uv sync --extra yubikey
goto :cleanup

:fail_token
echo.
echo FAIL: could not mint a token.
echo   - Is %PYEXE% runnable and the repo intact?
goto :cleanup

:fail_make
echo.
echo FAIL: make-credential did not return an ARKG seed key.
echo   - Is this console elevated? Run `sudo cmd`, then re-run this script.
echo     Unelevated processes see an empty FIDO device list, and the native
echo     WebAuthn path silently drops the previewSign output.
echo   - previewSign needs YubiKey firmware 5.8.0+.
echo   - A timeout here usually means the touch was missed; just run it again.
goto :cleanup

:fail_saved
echo.
echo FAIL: make-credential reported success but %CRED% was not written.
goto :cleanup

:fail_notyubikey
echo.
echo FAIL: --require-yubikey rejected this credential; nothing was registered.
echo       The reasons are listed above. Use --attest to register anyway, or pass
echo       --no-require-root to approver\responder_yubikey.py directly for a dev key.
set "RC=2"
goto :cleanup

:fail_register
echo.
echo FAIL: registration did not complete. Its own reason is printed just above.
echo   - "no responders" / a timeout: is NATS up?  cd nats ^&^& docker compose up -d
echo     ^(or the handler was not subscribed yet - just re-run^)
echo   - "no credential attestation object": that credential was created with
echo     attestation='none', so --attest / --require-yubikey have nothing to check.
echo   - "token unknown" / "expired": tokens live 15 min and are single-use; a
echo     re-run mints a fresh one, so simply run the script again.
echo   Re-running is cheap now - the credential is on disk, so no touch is needed:
echo       scripts\yubikey-approval.cmd --credential "%CRED%" --ctx %CTX%
goto :cleanup

:fail_allowlist
echo.
echo FAIL: the handler did not store a p256 key for %KEYID%.
echo       Without key_type=p256 in the allowlist the hook would verify with the
echo       wrong scheme, so this must never pass silently. Config: %HCFG%
goto :cleanup

:fail_hook
echo.
echo FAIL: hook.py did not print a signed decision ^(it fell back to the prompt^).
echo       Its own reason, verbatim:
if exist "%ERR%" type "%ERR%"
echo.
echo   - "no responders" means the responder was not subscribed yet; re-run with
echo     --credential "%CRED%" ^(no touch needed^).
echo   - "no reply within %HTIMEOUT%s" means the touch never arrived, or the
echo     responder could not sign; check the "%TITLE_R%" window.
echo   - Anything about the signature means the reply was not trusted - that is the
echo     interesting failure, and the responder window will say why.
goto :cleanup

:fail_decision
echo.
echo FAIL: the decision was not a verified %EXPECT%.
if exist "%OUT%" type "%OUT%"
goto :cleanup

:cleanup
REM Reap the background responder (serve runs forever) and any lingering handler.
taskkill /FI "WINDOWTITLE eq %TITLE_R%*" /T /F >nul 2>&1
taskkill /FI "WINDOWTITLE eq %TITLE_H%*" /T /F >nul 2>&1
del "%HCFG%" "%RCFG%" "%ANS%" "%PAYLOAD%" "%OUT%" "%ERR%" "%RUNRESP%" "%TOKFILE%" >nul 2>&1
echo.
if defined OWNCRED echo credential kept at: %CRED%
if not defined OWNCRED echo credential reused : %CRED%
echo re-run without spending a makeCredential touch:
echo   scripts\yubikey-approval.cmd --credential "%CRED%" --ctx %CTX% --rp-id %RPID%
if not "%RC%"=="0" echo.
if not "%RC%"=="0" echo ==== YUBIKEY APPROVAL FAILED ====
exit /b %RC%

:usage
echo Usage: scripts\yubikey-approval.cmd [--ctx LABEL] [--credential FILE]
echo            [--out FILE] [--rp-id ID] [--attest] [--require-yubikey]
echo            [--deny] [--timeout SEC]
echo.
echo   --ctx LABEL        derivation context label (default: ai-remote-approvals)
echo   --credential FILE  reuse a saved credential; skips step 2 and its touch
echo   --out FILE         where to keep the new credential (default: in TEMP)
echo   --rp-id ID         relying party of the credential (default: example.com)
echo   --attest           report the "is this really a YubiKey?" check at register
echo   --require-yubikey  imply --attest and refuse to register unless it verifies
echo   --deny             answer deny instead of allow, and expect a signed deny
echo   --timeout SEC      how long hook.py waits for the decision (default: 120)
echo.
echo Six steps: token, make-credential (touch), register, serve, hook (touch), verify.
echo Needs NATS on localhost. Run from an already-elevated console (`sudo cmd`).
exit /b 1
