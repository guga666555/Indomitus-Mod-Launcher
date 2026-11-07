@echo off
rem GEM ChildInject 0.1 build - Indomitus Launcher payload dll (injects the payload into call_to_arms_server.exe)
rem   build.bat          -> dist\gem_aa_childinject.dll
rem   build.bat payload  -> the above, then copies it into ..\..\launcher\payload\
rem   build.bat test     -> dist\test\: fake game + fake server, runs the spawn/inject test
rem zig: ..\..\zig\zig.exe, else zig on PATH, else python -m ziglang
setlocal EnableExtensions
cd /d "%~dp0"
if not exist dist mkdir dist
set "CCX="
if exist "%~dp0..\..\zig\zig.exe" set CCX="%~dp0..\..\zig\zig.exe" cc
if not defined CCX (where zig >nul 2>&1 && set CCX=zig cc)
if not defined CCX (python -c "import ziglang" >nul 2>&1 && set CCX=python -m ziglang cc)
if not defined CCX (echo ERROR: no zig found & exit /b 1)
set CCX=%CCX% -target x86_64-windows-gnu -mcpu=baseline -O2 -ffp-contract=off -fno-fast-math

if /i "%~1"=="test" goto :test
%CCX% -shared -s -o dist\gem_aa_childinject.dll src\gem_aa_childinject.c || exit /b 1
del dist\*.lib dist\*.pdb >nul 2>&1
echo built dist\gem_aa_childinject.dll
if /i not "%~1"=="payload" exit /b 0
copy /y dist\gem_aa_childinject.dll ..\..\launcher\payload\ >nul || exit /b 1
echo copied into launcher\payload - now rebuild the launcher
exit /b 0

:test
if not exist dist\test\payload mkdir dist\test\payload
%CCX% -shared -s -o dist\test\payload\gem_aa_childinject.dll src\gem_aa_childinject.c || exit /b 1
%CCX% -shared -s -o dist\test\payload\gem_dummy_a.dll test\dummy.c || exit /b 1
copy /y test\dummy.ini dist\test\payload\gem_dummy_a.ini >nul
%CCX% -municode -o dist\test\call_to_arms_citest.exe test\fake_game.c -lshell32 || exit /b 1
%CCX% -municode -o dist\test\call_to_arms_server.exe test\fake_server.c || exit /b 1
rem each fake server run must exit 2 (= gem_aa_childinject + gem_dummy_a loaded); cmd run exits 7
dist\test\call_to_arms_citest.exe "%~dp0dist\test\payload" "%~dp0dist\test\call_to_arms_server.exe"
type dist\test\payload\gem_aa_childinject.log
exit /b 0
