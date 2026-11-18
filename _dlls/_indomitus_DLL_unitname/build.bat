@echo off
rem GEM UnitName 0.1 build - Indomitus Launcher payload dll
rem   build.bat          -> dist\gem_unitname.dll + dist\gem_unitname.ini
rem   build.bat payload  -> the above, then copies both into ..\..\launcher\payload\
rem   build.bat test     -> dist\un_test.exe: finders on the game / ed exe (one exe per run) + the rule on fake objects
rem                         (binaries\x64 folder: 2nd argument, else GOH_BIN)
rem zig: ..\..\zig\zig.exe, else zig on PATH, else python -m ziglang
rem Flags = the build Alex tested in game (Nov 17 2026).
setlocal EnableExtensions
cd /d "%~dp0"
if not exist dist mkdir dist
set "CCX="
if exist "%~dp0..\..\zig\zig.exe" set CCX="%~dp0..\..\zig\zig.exe" cc
if not defined CCX (where zig >nul 2>&1 && set CCX=zig cc)
if not defined CCX (python -c "import ziglang" >nul 2>&1 && set CCX=python -m ziglang cc)
if not defined CCX (echo ERROR: no zig found & exit /b 1)
set CCX=%CCX% -target x86_64-windows-gnu -mcpu=baseline -O2 -Wall -Wextra

if /i "%~1"=="test" goto :test
%CCX% -shared -s -o dist\gem_unitname.dll src\gem_unitname.c || exit /b 1
copy /y src\gem_unitname.ini dist\ >nul
del dist\*.lib dist\*.pdb >nul 2>&1
echo built dist\gem_unitname.dll
if /i not "%~1"=="payload" exit /b 0
copy /y dist\gem_unitname.dll ..\..\launcher\payload\ >nul || exit /b 1
copy /y dist\gem_unitname.ini ..\..\launcher\payload\ >nul || exit /b 1
echo copied into launcher\payload - now rebuild the launcher
exit /b 0

:test
set "BIN=%GOH_BIN%"
if not "%~2"=="" set "BIN=%~2"
if "%BIN%"=="" (echo pass the binaries\x64 folder or set GOH_BIN & exit /b 1)
%CCX% -DUN_TEST -Wno-unused-variable -Wno-unused-function -o dist\un_test.exe src\gem_unitname.c || exit /b 1
dist\un_test.exe "%BIN%\call_to_arms.exe" || exit /b 1
dist\un_test.exe "%BIN%\call_to_arms_ed.exe" || exit /b 1
exit /b 0
