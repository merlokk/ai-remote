@echo off
setlocal enableextensions

REM ===========================================================================
REM  The ESP32 host tier -- tier 1 of approver-esp32\tests.md section 10.11:
REM  every test in that firmware that needs no board, built by the host compiler
REM  and run in one binary.
REM
REM  It is here because every other tier in this repository has a way in from
REM  scripts\ and this one did not: CI runs it, and a developer still had to know
REM  that approver-esp32\host_test\run.cmd exists. This is a launcher and
REM  deliberately nothing more -- the build lives in that file, next to the
REM  CMakeLists.txt it drives.
REM
REM  Usage: scripts\esp32-host-tests.cmd [filter ...]
REM
REM  With no arguments it runs everything, which is what a pre-commit check
REM  wants. Arguments are suite filters -- a suite runs when its name contains
REM  one of them -- which is what a debugging loop wants:
REM
REM    scripts\esp32-host-tests.cmd              REM all of it
REM    scripts\esp32-host-tests.cmd web auth     REM the configuration site's
REM    scripts\esp32-host-tests.cmd i2c pmic     REM the bus and the chip on it
REM
REM  Needs no NATS, no board and no Python. It does need MSVC, CMake and Ninja,
REM  all of which are on this machine for other reasons (approver-esp32\build.md
REM  section 10.12), plus an ESP-IDF checkout for Unity's sources and the cJSON
REM  managed component -- run.cmd resolves both and says so when it cannot.
REM
REM  Exit codes: 0 = every test passed, 1 = a test failed, or the build did.
REM ===========================================================================

set "ROOT=%~dp0.."
set "RUNNER=%ROOT%\approver-esp32\host_test\run.cmd"

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
echo Usage: scripts\esp32-host-tests.cmd [filter ...]
echo.
echo Runs the ESP32 host tier (approver-esp32\tests.md section 10.11) -- the
echo tests that need no board. No arguments runs all of them; an argument is a
echo suite filter.
echo.
echo    scripts\esp32-host-tests.cmd
echo    scripts\esp32-host-tests.cmd web auth
echo.
exit /b 1
