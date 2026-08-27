@echo off
setlocal enableextensions

REM ===========================================================================
REM  The host tier of the *second* ESP32 firmware -- tier 1 of
REM  approver-esp32-yubikey\tests.md section 10.11: every test in that firmware
REM  that needs no board, built by the host compiler and run in one binary.
REM
REM  This is esp32-host-tests.cmd's twin and deliberately identical in shape.
REM  Two boards, two host tiers, two launchers -- because the alternative is one
REM  launcher with a board argument, and a pre-commit check that has to be told
REM  which half of the repository it is checking is a check that gets run on one
REM  half.
REM
REM  Usage: scripts\esp32yk-host-tests.cmd [filter ...]
REM
REM  With no arguments it runs everything, which is what a pre-commit check
REM  wants. Arguments are suite filters -- a suite runs when its name contains
REM  one of them -- which is what a debugging loop wants:
REM
REM    scripts\esp32yk-host-tests.cmd                REM all 466 of them
REM    scripts\esp32yk-host-tests.cmd led indicator  REM the light and its ranking
REM    scripts\esp32yk-host-tests.cmd fido           REM ctaphid + cbor + ctap2
REM
REM  Needs no NATS, no board, no security key and no Python. It does need MSVC,
REM  CMake and Ninja, all of which are on this machine for other reasons
REM  (approver-esp32-yubikey\build.md section 10.4), plus an ESP-IDF checkout for
REM  Unity's sources and the cJSON managed component -- run.cmd resolves both and
REM  says so when it cannot.
REM
REM  One thing it needs that its twin does not: the parity vectors, which live
REM  in approver-esp32\host_test\vectors\ and are read from there rather than
REM  copied. There is one copy of that header in the repository on purpose --
REM  approver-esp32-yubikey\tests.md section 10.11 says why.
REM
REM  Exit codes: 0 = every test passed, 1 = a test failed, or the build did.
REM ===========================================================================

set "ROOT=%~dp0.."
set "RUNNER=%ROOT%\approver-esp32-yubikey\host_test\run.cmd"

if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-h" goto :usage

if not exist "%RUNNER%" goto :no_runner

call "%RUNNER%" %*
exit /b %ERRORLEVEL%

:no_runner
echo FAIL: %RUNNER% not found.
exit /b 1

:usage
echo.
echo Usage: scripts\esp32yk-host-tests.cmd [filter ...]
echo.
echo Runs the host tier of the ESP32-S3 + security key firmware
echo (approver-esp32-yubikey\tests.md section 10.11) -- the tests that need no
echo board. No arguments runs all of them; an argument is a suite filter.
echo.
echo    scripts\esp32yk-host-tests.cmd
echo    scripts\esp32yk-host-tests.cmd led indicator
echo    scripts\esp32yk-host-tests.cmd fido
echo.
exit /b 1
