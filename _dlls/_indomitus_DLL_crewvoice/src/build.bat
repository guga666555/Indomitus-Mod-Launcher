@echo off
rem GEM CrewVoice build (zig cc: set ZIG_EXE to a zig.exe, or pip install ziglang)
rem   build.bat        -> out\gem_crewvoice.dll (+ gem_inject.exe copied from gem_meleestance\out)
rem   build.bat test   -> out\cv_test.exe: finders + the engine's TagCopy on the three game exes (one exe per process), fallback logic, asm stub
setlocal
cd /d "%~dp0"
if not exist out mkdir out
set ZIG=python -m ziglang
if defined ZIG_EXE set ZIG="%ZIG_EXE%"
set CC=%ZIG% cc -target x86_64-windows-gnu -mcpu=baseline -O2 -Wall -Wextra
if /i "%1"=="test" (
  %CC% -DCV_TEST -o out\cv_test.exe gem_crewvoice.c || exit /b 1
  out\cv_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms.exe" || exit /b 1
  out\cv_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms_ed.exe" || exit /b 1
  out\cv_test.exe "%~dp0..\..\..\..\binaries\x64\call_to_arms_server.exe" || exit /b 1
  exit /b 0
)
%CC% -shared -s -o out\gem_crewvoice.dll gem_crewvoice.c || exit /b 1
if exist ..\gem_meleestance\out\gem_inject.exe copy /y ..\gem_meleestance\out\gem_inject.exe out\ >nul
copy /y gem_crewvoice.ini out\ >nul
copy /y inject.bat out\ >nul
copy /y README.md out\ >nul
echo built out\gem_crewvoice.dll
