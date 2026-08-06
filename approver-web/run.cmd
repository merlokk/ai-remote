@echo off
setlocal enableextensions enabledelayedexpansion

REM ===========================================================================
REM  Start the approver-web server (approver-web\CLAUDE.md).
REM
REM  Opening the page IS being a responder: the server subscribes to approvals.*
REM  in the `approvers` queue group as soon as a browser opens the SSE stream.
REM  So run only ONE responder at a time -- with responder_yubikey.py also up,
REM  each request reaches exactly one of them, arbitrarily.
REM
REM  Installs dependencies on the first run (node_modules missing) and warns,
REM  without stopping, if nothing is listening on the NATS client port: the app
REM  starts fine and reports the failure in its own status bar.
REM
REM  Unlike the scripts in scripts\, this needs NO elevation and no venv -- it is
REM  the Node half of the repo and touches no FIDO device.
REM
REM  Usage: run.cmd [--prod] [--port N] [--install] [--help]
REM    --prod      build once, then serve the production build (default: dev)
REM    --port N    listen on N (default: 3000)
REM    --install   re-run `npm install` even when node_modules already exists
REM
REM  Exit codes: 0 = the server exited cleanly (Ctrl+C), 1 = it could not start.
REM
REM  NOTE ON STRUCTURE: no `if` here opens a parenthesised block, and the file is
REM  CRLF -- same reason as scripts\yubikey-approval.cmd, where cmd resumed at the
REM  wrong byte offset after a goto out of a block and skipped whole if-blocks.
REM ===========================================================================

set "APP=%~dp0"
set "MODE=dev"
set "PORT=3000"
set "FORCEINSTALL="

REM --- argument parsing: one single-line `if` per option, bodies at labels ------
:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="--prod" goto :opt_prod
if /i "%~1"=="--port" goto :opt_port
if /i "%~1"=="--install" goto :opt_install
if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-h" goto :usage
echo unknown argument: %~1
goto :usage

:opt_prod
set "MODE=prod"
shift
goto :parse

:opt_port
set "PORT=%~2"
shift
shift
goto :parse

:opt_install
set "FORCEINSTALL=1"
shift
goto :parse

:usage
echo.
echo Usage: run.cmd [--prod] [--port N] [--install] [--help]
echo    --prod      build once, then serve the production build (default: dev)
echo    --port N    listen on N (default: 3000)
echo    --install   re-run `npm install` even when node_modules already exists
echo.
exit /b 1

:parsed
cd /d "%APP%"

where node >nul 2>&1
if errorlevel 1 goto :no_node
where npm >nul 2>&1
if errorlevel 1 goto :no_node

REM --- dependencies ------------------------------------------------------------
if defined FORCEINSTALL goto :install
if not exist "%APP%node_modules" goto :install
goto :deps_ok

:install
echo [1/3] npm install
call npm install --no-audit --no-fund
if errorlevel 1 goto :install_failed
goto :deps_ok

:deps_ok
echo [1/3] dependencies ok

REM --- NATS reachable? ---------------------------------------------------------
REM Only a warning: the app starts either way and says so in its status bar.
netstat -an | findstr ":4222" | findstr "LISTENING" >nul 2>&1
if errorlevel 1 echo [2/3] warning: nothing is listening on port 4222 - start NATS with: cd ..\nats ^&^& docker compose up -d
if not errorlevel 1 echo [2/3] NATS port 4222 is listening

REM --- run ---------------------------------------------------------------------
if /i "%MODE%"=="prod" goto :run_prod

echo [3/3] starting the dev server on http://localhost:%PORT%/ - Ctrl+C to stop
echo.
call npm run dev -- --port %PORT%
exit /b %ERRORLEVEL%

:run_prod
echo [3/3] building
call npm run build
if errorlevel 1 goto :build_failed
echo.
echo serving the production build on http://localhost:%PORT%/ - Ctrl+C to stop
echo.
call npm run start -- --port %PORT%
exit /b %ERRORLEVEL%

REM --- failures ----------------------------------------------------------------
:no_node
echo FAIL: node/npm not found on PATH.
echo       Install Node.js (this app is the only part of the repo that needs it).
exit /b 1

:install_failed
echo FAIL: npm install failed - see the output above.
exit /b 1

:build_failed
echo FAIL: the production build failed - see the output above.
echo       `run.cmd` without --prod starts the dev server, which is more forgiving.
exit /b 1
