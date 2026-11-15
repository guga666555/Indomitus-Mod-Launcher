@echo off
rem GEM UserControl build - Indomitus Launcher payload dll
rem   build.bat          -> out\gem_usercontrol.dll + out\gem_usercontrol.ini
rem   build.bat payload  -> the above, then copies both into launcher\payload\ (then run build_zig.bat in the launcher folder)
rem   build.bat test     -> out\uc_test.exe: finders on the three game exes + logic tests
rem                         (binaries\x64 folder: 2nd argument, else GOH_BIN)
rem   build.bat inject   -> out\gem_inject.exe (in-house test helper, not shipped): loads a dll into a running game or editor
rem zig: %ZIG_EXE% if set, else the launcher's zig\zig.exe (build_zig.bat downloads it), else zig on PATH, else python -m ziglang
setlocal EnableExtensions
cd /d "%~dp0"
if not exist out mkdir out
set "ROOT=%~dp0..\..\.."
set "CCX="
if defined ZIG_EXE set CCX="%ZIG_EXE%" cc
if not defined CCX if exist "%ROOT%\zig\zig.exe" set CCX="%ROOT%\zig\zig.exe" cc
if not defined CCX (where zig >nul 2>&1 && set CCX=zig cc)
if not defined CCX (python -c "import ziglang" >nul 2>&1 && set CCX=python -m ziglang cc)
if not defined CCX (echo ERROR: no zig found & exit /b 1)
set CCX=%CCX% -target x86_64-windows-gnu -mcpu=baseline -O2

if /i "%~1"=="test" goto :test
if /i "%~1"=="inject" goto :inject
%CCX% -shared -s -o out\gem_usercontrol.dll gem_usercontrol.c || exit /b 1
copy /y gem_usercontrol.ini out\ >nul
del out\*.lib out\*.pdb >nul 2>&1
echo built out\gem_usercontrol.dll
if /i not "%~1"=="payload" exit /b 0
copy /y out\gem_usercontrol.dll "%ROOT%\launcher\payload\" >nul || exit /b 1
copy /y out\gem_usercontrol.ini "%ROOT%\launcher\payload\" >nul || exit /b 1
echo copied into launcher\payload - now rebuild the launcher
exit /b 0

:inject
rem SquadFollow's injector source; the dll to load is its argument
%CCX% -s -municode -o out\gem_inject.exe "%ROOT%\_dlls\_indomitus_DLL_squadfollow\src\gem_inject.c" || exit /b 1
echo built out\gem_inject.exe - game or editor in the main menu, then from out\:  gem_inject.exe gem_usercontrol.dll
exit /b 0

:test
set "BIN=%GOH_BIN%"
if not "%~2"=="" set "BIN=%~2"
if "%BIN%"=="" (echo pass the binaries\x64 folder or set GOH_BIN & exit /b 1)
%CCX% -DUC_TEST -o out\uc_test.exe gem_usercontrol.c || exit /b 1
rem one exe per process: a second exe mapped into the same process is not relocated
out\uc_test.exe "%BIN%\call_to_arms.exe" || exit /b 1
out\uc_test.exe "%BIN%\call_to_arms_ed.exe" || exit /b 1
out\uc_test.exe "%BIN%\call_to_arms_server.exe" || exit /b 1
exit /b 0
