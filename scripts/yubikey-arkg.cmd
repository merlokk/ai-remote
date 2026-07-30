@echo off
setlocal enableextensions enabledelayedexpansion

REM ===========================================================================
REM  YubiKey ARKG run against real hardware (CLAUDE.md 8).
REM
REM    1. version           - firmware / AAGUID / previewSign support (no touch)
REM    2. make-credential   - an ARKG seed key on the device (NEEDS A TOUCH)
REM    3. derive            - the public key + arkg_args out of that credential,
REM                          or, with --message, derive + sign that string with the
REM                          derived key and verify the signature (SECOND TOUCH)
REM
REM  Unlike scripts\e2e-*.cmd this is not a self-checking test: step 2 needs a
REM  human finger, so it drives the real tools\yubikey_exec.py and reports what
REM  the key returned.
REM
REM  ELEVATION: run this from an ALREADY elevated console -- e.g. `sudo cmd`,
REM  then this script. It does not elevate itself. Windows hands FIDO devices to
REM  the WebAuthn API only, so an unelevated process sees no key at all.
REM  Because elevating drops VIRTUAL_ENV, this script calls the venv interpreter
REM  by path instead of relying on the `py` launcher.
REM
REM  Usage: scripts\yubikey-arkg.cmd [--ctx LABEL] [--out FILE] [--attest]
REM                                  [--message STRING]
REM    --ctx LABEL       derivation context label (default: ai-remote)
REM    --out FILE        where to keep the credential (default: a file in %TEMP%)
REM    --attest          also run the optional "is this really a YubiKey?" check
REM    --message STRING  sign this string with the derived key and verify it
REM
REM  The credential file is KEPT on purpose: `yubikey_exec.py derive --in <file>`
REM  re-derives from it later with the key unplugged. It holds no private key
REM  material -- the ARKG private halves never leave the device.
REM
REM  Exit code: 0 = all three steps OK, 1 = a step failed.
REM ===========================================================================

set "ROOT=%~dp0.."
set "TOOL=%ROOT%\tools\yubikey_exec.py"
set "CTX=ai-remote"
set "CRED=%TEMP%\airemote-yubikey-cred-%RANDOM%%RANDOM%.json"
set "ATTEST="
set "MSG="

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
if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-h" goto :usage
echo unknown argument: %~1
goto :usage
:parsed

if not exist "%TOOL%" (
    echo FAIL: cannot find %TOOL%
    exit /b 1
)

echo ==== YubiKey ARKG run ====
echo   interpreter : %PYEXE%
echo   ctx         : %CTX%
echo   credential  : %CRED%
if defined MSG echo   message     : %MSG%
echo.

echo [1/3] reading the device version ...
"%PYEXE%" "%TOOL%" version
if errorlevel 1 goto :fail_version
echo.

echo [2/3] make-credential -- TOUCH YOUR YUBIKEY WHEN IT BLINKS ...
"%PYEXE%" "%TOOL%" make-credential --out "%CRED%" %ATTEST%
if errorlevel 1 goto :fail_make
if not exist "%CRED%" goto :fail_saved
echo.

if defined MSG goto :step3_sign

echo [3/3] deriving the public key from that credential ...
"%PYEXE%" "%TOOL%" derive --in "%CRED%" --ctx "%CTX%"
if errorlevel 1 goto :fail_derive
goto :done

:step3_sign
echo [3/3] deriving, then signing the message -- SECOND TOUCH NEEDED ...
"%PYEXE%" "%TOOL%" sign --in "%CRED%" --ctx "%CTX%" --message "%MSG%"
if errorlevel 3 goto :fail_signature
if errorlevel 1 goto :fail_sign

:done
echo.
echo ==== DONE ====
echo credential kept at: %CRED%
echo re-derive any time (no key needed):
echo   "%PYEXE%" "%TOOL%" derive --in "%CRED%" --ctx %CTX%
if defined MSG (
    echo verify a signature offline with the ikm printed above:
    echo   "%PYEXE%" "%TOOL%" verify --in "%CRED%" --ctx %CTX% --ikm ^<hex^> --message "%MSG%" --signature ^<hex^>
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
exit /b 1

:usage
echo Usage: scripts\yubikey-arkg.cmd [--ctx LABEL] [--out FILE] [--attest]
echo                                 [--message STRING]
echo.
echo   --ctx LABEL       derivation context label (default: ai-remote)
echo   --out FILE        where to keep the credential (default: a file in TEMP)
echo   --attest          also run the optional "is this really a YubiKey?" check
echo   --message STRING  sign this string with the derived key, then verify it
echo                     (costs a second touch)
echo.
echo Run from an already-elevated console (`sudo cmd`, then this script).
exit /b 1
