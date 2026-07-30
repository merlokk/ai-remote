@echo off
setlocal enableextensions enabledelayedexpansion

REM ===========================================================================
REM  YubiKey ARKG run against real hardware (CLAUDE.md 8).
REM
REM    1. version           - firmware / AAGUID / previewSign support (no touch)
REM    2. make-credential   - an ARKG seed key on the device (FIRST TOUCH)
REM    3. derive            - a public key + arkg_args out of that credential
REM    4. sign              - sign the message with the derived key (SECOND TOUCH)
REM    5. verify            - check that signature offline, no device involved
REM
REM  Steps 3-5 must all address the SAME derived key, so this script generates one
REM  ikm up front (32 random bytes, via os.urandom) and passes --ikm to each of
REM  them. Without that, `derive` and `sign` would each roll their own ikm and end
REM  up with different, unrelated keys. Step 4 writes the signature to a file with
REM  --sig-out so step 5 can read it back.
REM
REM  Step 4 already verifies inline (the tool always does, as a sanity check while
REM  both halves are in hand). Step 5 is the interesting one: it re-derives the
REM  public key from the credential file alone and verifies with NO key attached --
REM  the "verify later, elsewhere" path.
REM
REM  Unlike scripts\e2e-*.cmd this is not a self-checking test: it needs a human
REM  finger, so it drives the real tools\yubikey_exec.py and reports what the key
REM  returned.
REM
REM  ELEVATION: run this from an ALREADY elevated console -- e.g. `sudo cmd`,
REM  then this script. It does not elevate itself. Windows hands FIDO devices to
REM  the WebAuthn API only, so an unelevated process sees no key at all.
REM  Because elevating drops VIRTUAL_ENV, this script calls the venv interpreter
REM  by path instead of relying on the `py` launcher.
REM
REM  Usage: scripts\yubikey-arkg.cmd [--ctx LABEL] [--out FILE] [--attest]
REM                                  [--message STRING] [--ikm HEX] [--no-sign]
REM    --ctx LABEL       derivation context label (default: ai-remote)
REM    --out FILE        where to keep the credential (default: a file in %TEMP%)
REM    --attest          also run the optional "is this really a YubiKey?" check
REM    --message STRING  what to sign (default: a fixed smoke-test string)
REM    --ikm HEX         reuse a specific ikm instead of a fresh random one
REM    --no-sign         stop after step 3; only one touch, no signing
REM
REM  The credential file is KEPT on purpose: `yubikey_exec.py derive --in <file>`
REM  re-derives from it later with the key unplugged. It holds no private key
REM  material -- the ARKG private halves never leave the device.
REM
REM  Exit codes: 0 = all steps OK, 1 = a step failed, 3 = the key signed but the
REM  signature does not verify (a real mismatch, not a missed touch).
REM ===========================================================================

set "ROOT=%~dp0.."
set "TOOL=%ROOT%\tools\yubikey_exec.py"
set "CTX=ai-remote"
set "SFX=%RANDOM%%RANDOM%"
set "CRED=%TEMP%\airemote-yubikey-cred-%SFX%.json"
set "SIG=%TEMP%\airemote-yubikey-sig-%SFX%.txt"
set "IKMFILE=%TEMP%\airemote-yubikey-ikm-%SFX%.txt"
set "ATTEST="
set "MSG=ai-remote yubikey-arkg smoke test"
set "IKM="
set "NOSIGN="

REM Prefer the project venv (elevation loses VIRTUAL_ENV, so `py` would pick the
REM global interpreter, which has neither cryptography nor fido2).
set "PYEXE=%ROOT%\.venv\Scripts\python.exe"
if not exist "%PYEXE%" (
    echo warning: %ROOT%\.venv not found, falling back to the `py` launcher
    set "PYEXE=py"
)

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="--ctx" (
    set "CTX=%~2"
    shift
    shift
    goto :parse
)
if /i "%~1"=="--out" (
    set "CRED=%~2"
    shift
    shift
    goto :parse
)
if /i "%~1"=="--attest" (
    set "ATTEST=--attest"
    shift
    goto :parse
)
if /i "%~1"=="--message" (
    set "MSG=%~2"
    shift
    shift
    goto :parse
)
if /i "%~1"=="--ikm" (
    set "IKM=%~2"
    shift
    shift
    goto :parse
)
if /i "%~1"=="--no-sign" (
    set "NOSIGN=1"
    shift
    goto :parse
)
if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-h" goto :usage
echo unknown argument: %~1
goto :usage
:parsed

if not exist "%TOOL%" (
    echo FAIL: cannot find %TOOL%
    exit /b 1
)

if defined NOSIGN (set "STEPS=3") else (set "STEPS=5")

REM One ikm for steps 3-5, so they all speak about the same derived key. Written to a
REM file and read back rather than captured with `for /f usebackq`: nested quotes in a
REM backquoted command get mangled by cmd, which silently truncates the -c argument.
if not defined IKM (
    "%PYEXE%" -c "import os;print(os.urandom(32).hex())" > "%IKMFILE%" 2>nul
    if exist "%IKMFILE%" for /f "usebackq delims=" %%i in ("%IKMFILE%") do set "IKM=%%i"
    del "%IKMFILE%" >nul 2>&1
)
if not defined IKM (
    echo FAIL: could not generate an ikm -- is %PYEXE% runnable?
    echo   tried: "%PYEXE%" -c "import os;print(os.urandom(32).hex())"
    exit /b 1
)

echo ==== YubiKey ARKG run ====
echo   interpreter : %PYEXE%
echo   ctx         : %CTX%
echo   ikm         : %IKM%
echo   credential  : %CRED%
if not defined NOSIGN echo   message     : %MSG%
echo.

echo [1/%STEPS%] reading the device version ...
"%PYEXE%" "%TOOL%" version
if errorlevel 1 goto :fail_version
echo.

echo [2/%STEPS%] make-credential -- TOUCH YOUR YUBIKEY WHEN IT BLINKS ...
"%PYEXE%" "%TOOL%" make-credential --out "%CRED%" %ATTEST%
if errorlevel 1 goto :fail_make
if not exist "%CRED%" goto :fail_saved
echo.

echo [3/%STEPS%] deriving the public key from that credential ...
"%PYEXE%" "%TOOL%" derive --in "%CRED%" --ctx "%CTX%" --ikm "%IKM%"
if errorlevel 1 goto :fail_derive
echo.

if defined NOSIGN goto :done

echo [4/%STEPS%] signing the message -- SECOND TOUCH NEEDED ...
"%PYEXE%" "%TOOL%" sign --in "%CRED%" --ctx "%CTX%" --ikm "%IKM%" --message "%MSG%" --sig-out "%SIG%"
if errorlevel 3 goto :fail_signature
if errorlevel 1 goto :fail_sign
if not exist "%SIG%" goto :fail_sig_saved
echo.

set "SIG_HEX="
for /f "usebackq delims=" %%s in ("%SIG%") do set "SIG_HEX=%%s"
if not defined SIG_HEX goto :fail_sig_saved

echo [5/%STEPS%] verifying that signature offline -- no device needed ...
"%PYEXE%" "%TOOL%" verify --in "%CRED%" --ctx "%CTX%" --ikm "%IKM%" --message "%MSG%" --signature "!SIG_HEX!"
if errorlevel 3 goto :fail_verify_mismatch
if errorlevel 1 goto :fail_verify
echo.

:done
echo.
echo ==== DONE ====
echo credential kept at: %CRED%
if not defined NOSIGN echo signature kept at : %SIG%
echo.
echo reproduce the same derived key any time (no key needed):
echo   "%PYEXE%" "%TOOL%" derive --in "%CRED%" --ctx %CTX% --ikm %IKM%
if not defined NOSIGN (
    echo re-verify the signature offline:
    echo   "%PYEXE%" "%TOOL%" verify --in "%CRED%" --ctx %CTX% --ikm %IKM% --message "%MSG%" --signature !SIG_HEX!
)
exit /b 0

:fail_version
echo.
echo FAIL: could not read the device.
echo   - Is this console elevated? Run `sudo cmd`, then re-run this script.
echo     Unelevated processes see an empty FIDO device list even with a key in.
echo   - Is the YubiKey plugged in?
echo   - Is the extra installed?  uv sync --extra yubikey
exit /b 1

:fail_make
echo.
echo FAIL: make-credential did not return an ARKG seed key.
echo   - previewSign needs YubiKey firmware 5.8.0+ (see step 1 output above).
echo   - On Windows an unelevated run uses the native WebAuthn path, which
echo     silently drops the previewSign output. Elevate and retry.
echo   - A timeout here usually means the touch was missed; just run it again.
exit /b 1

:fail_saved
echo.
echo FAIL: make-credential reported success but %CRED% was not written.
exit /b 1

:fail_derive
echo.
echo FAIL: could not derive from %CRED%.
exit /b 1

:fail_sign
echo.
echo FAIL: signing did not complete.
echo   - A timeout usually means the second touch was missed; run it again.
echo   - previewSign must be supported (see step 1 output above).
exit /b 1

:fail_signature
echo.
echo FAIL: the key produced a signature, but it does NOT verify against the
echo       derived public key. That is a real mismatch, not a missed touch.
exit /b 3

:fail_sig_saved
echo.
echo FAIL: signing succeeded but no signature was written to %SIG%.
exit /b 1

:fail_verify
echo.
echo FAIL: the offline verify step could not run.
echo   - It re-derives from %CRED% with --ikm %IKM%; both must still be readable.
exit /b 1

:fail_verify_mismatch
echo.
echo FAIL: the signature did NOT verify offline, even though step 4 accepted it.
echo       That points at the ctx/ikm used to re-derive, not at the key itself.
exit /b 3

:usage
echo Usage: scripts\yubikey-arkg.cmd [--ctx LABEL] [--out FILE] [--attest]
echo                                 [--message STRING] [--ikm HEX] [--no-sign]
echo.
echo   --ctx LABEL       derivation context label (default: ai-remote)
echo   --out FILE        where to keep the credential (default: a file in TEMP)
echo   --attest          also run the optional "is this really a YubiKey?" check
echo   --message STRING  what to sign (default: a fixed smoke-test string)
echo   --ikm HEX         reuse a specific ikm instead of a fresh random one
echo   --no-sign         stop after step 3; one touch only, no signing
echo.
echo Five steps: version, make-credential (touch), derive, sign (touch), verify.
echo Run from an already-elevated console (`sudo cmd`, then this script).
exit /b 1
