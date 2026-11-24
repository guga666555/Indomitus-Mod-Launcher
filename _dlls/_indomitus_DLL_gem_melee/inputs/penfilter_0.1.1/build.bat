@echo off
rem GEM PenFilter build (zig cc: the launcher's zig, or `pip install ziglang`)
rem   build.bat        -> out\gem_penfilter.dll (+ ini, inject.bat; gem_inject.exe is the prebuilt copy from gem_itempen)
rem   build.bat test   -> out\pf_test.exe, run on the three game exes; output also written to out\selftest.txt
setlocal
cd /d "%~dp0"
if not exist out mkdir out
set ZIG=C:\Users\alex_\Documents\IndomitusLauncher\zig\zig.exe
if exist "%ZIG%" (set CC="%ZIG%" cc) else (set CC=python -m ziglang cc)
set CC=%CC% -target x86_64-windows-gnu -mcpu=baseline -O2 -ffp-contract=off -fno-fast-math
set GOH=C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell\binaries\x64
if /i "%1"=="test" goto test
%CC% -shared -s -o out\gem_penfilter.dll gem_penfilter.c || exit /b 1
copy /y gem_penfilter.ini out\ >nul
copy /y inject.bat out\ >nul
echo built out\gem_penfilter.dll
exit /b 0

:test
%CC% -DPF_TEST -o out\pf_test.exe gem_penfilter.c || exit /b 1
out\pf_test.exe "%GOH%\call_to_arms.exe" "%GOH%\call_to_arms_ed.exe" "%GOH%\call_to_arms_server.exe" > out\selftest.txt
type out\selftest.txt
findstr /x /c:"ALL OK" out\selftest.txt >nul
exit /b %errorlevel%
