@echo off
setlocal

rem Build and run the host tier of CLAUDE.md section 10.11 - the tests that
rem need no board. CMakeLists.txt says why this is not
rem `idf.py --preview set-target linux`: that target is offered by this install
rem and does not work on a Windows host.
rem
rem The three tools it reaches for are already on this machine for other
rem reasons - MSVC because the LVGL preview of section 10.12.1 compiles
rem snippets with it, CMake and Ninja because ESP-IDF ships them. Nothing new
rem is installed in order to run a test.
rem
rem Kept ASCII and CRLF on purpose: cmd.exe parses a UTF-8 em dash as a command.

set "VCVARSALL=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
set "CMAKE=C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe"
set "NINJA=C:\Espressif\tools\ninja\1.12.1\ninja.exe"
if "%IDF_PATH%"=="" set "IDF_PATH=E:\esp\v6.0.2\esp-idf"

rem `goto` rather than a parenthesised `if` block: the path holds `(x86)`, and
rem cmd.exe closes the block on that first bracket. The symptom is
rem "\Microsoft was unexpected at this time."
if not exist "%VCVARSALL%" goto :novs

call "%VCVARSALL%" x64 >nul || exit /b 1

pushd "%~dp0"
"%CMAKE%" -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DIDF_PATH="%IDF_PATH%" || goto :fail
"%CMAKE%" --build build || goto :fail
build\host_test.exe %* || goto :fail
popd
exit /b 0

:fail
popd
exit /b 1

:novs
echo Visual Studio build tools not found at:
echo   %VCVARSALL%
echo working-with-code.md records this machine's path - update both if it moved.
exit /b 1
