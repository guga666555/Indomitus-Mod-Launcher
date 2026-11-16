@echo off
rem GEM HeightmapGuard build (zig cc: set ZIG_EXE to a zig.exe, or pip install ziglang)
rem   build.bat        -> out\gem_hmapguard.dll
rem   build.bat test   -> out\hg_test.exe: finders + the engine's own restore / apply loops on a faked grid, on the three game exes
setlocal
cd /d "%~dp0"
if not exist out mkdir out
set ZIG=python -m ziglang
if defined ZIG_EXE set ZIG="%ZIG_EXE%"
set CC=%ZIG% cc -target x86_64-windows-gnu -mcpu=baseline -O2 -Wall -Wextra
set BIN=C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell\binaries\x64
if /i "%1"=="test" (
  %CC% -DHG_TEST -o out\hg_test.exe gem_hmapguard.c || exit /b 1
  out\hg_test.exe "%BIN%\call_to_arms.exe" || exit /b 1
  out\hg_test.exe "%BIN%\call_to_arms_ed.exe" || exit /b 1
  out\hg_test.exe "%BIN%\call_to_arms_server.exe" || exit /b 1
  exit /b 0
)
%CC% -shared -s -o out\gem_hmapguard.dll gem_hmapguard.c || exit /b 1
copy /y gem_hmapguard.ini out\ >nul
echo built out\gem_hmapguard.dll
