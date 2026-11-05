@echo off
rem GEM SquadFollow 0.2 build - Indomitus Launcher payload dll
rem   build.bat          -> dist\gem_squadfollow.dll + dist\gem_squadfollow.ini
rem   build.bat payload  -> the above, then copies both into ..\..\launcher\payload\
rem   build.bat test     -> dist\sf_test.exe, run on the game / editor / server exe
rem                         folder: 2nd argument, else %%GOH_BIN%%, else the F:\SteamLibrary install
rem   build.bat inject   -> dist\gem_inject.exe (in-house runtime test helper, not shipped)
rem zig: ..\..\zig\zig.exe (the launcher repo's portable zig), else zig on PATH, else python -m ziglang
rem Everything is also written to build_log.txt next to this file.
setlocal EnableExtensions
cd /d "%~dp0"
set "LOG=%~dp0build_log.txt"
> "%LOG%" echo GEM SquadFollow build %DATE% %TIME% [%*]
if not exist dist mkdir dist

set "CCX="
if exist "%~dp0..\..\zig\zig.exe" set CCX="%~dp0..\..\zig\zig.exe" cc
if not defined CCX (where zig >nul 2>&1 && set CCX=zig cc)
if not defined CCX (python -c "import ziglang" >nul 2>&1 && set CCX=python -m ziglang cc)
if not defined CCX (
  >> "%LOG%" echo ERROR: no zig found ^(..\..\zig\zig.exe, PATH, python -m ziglang^)
  goto :fail
)
rem -mcpu=baseline -ffp-contract=off -fno-fast-math: no AVX/FMA, identical float results on every
rem CPU (lockstep multiplayer). Do not change these.
set CCX=%CCX% -target x86_64-windows-gnu -mcpu=baseline -O2 -ffp-contract=off -fno-fast-math
>> "%LOG%" echo compiler: %CCX%

if /i "%~1"=="test" goto :test
if /i "%~1"=="inject" goto :inject

call :cc -shared -s -o dist\gem_squadfollow.dll src\gem_squadfollow.c || goto :fail
copy /y src\gem_squadfollow.ini dist\ >nul || goto :fail
>> "%LOG%" echo built dist\gem_squadfollow.dll + dist\gem_squadfollow.ini
if /i not "%~1"=="payload" goto :ok

set "PAY=%~dp0..\..\launcher\payload"
if not exist "%PAY%\" (
  >> "%LOG%" echo ERROR: launcher payload folder not found: %PAY%
  goto :fail
)
copy /y dist\gem_squadfollow.dll "%PAY%\" >nul || goto :fail
copy /y dist\gem_squadfollow.ini "%PAY%\" >nul || goto :fail
>> "%LOG%" echo copied into %PAY% - now rebuild the launcher ^(build_zig.bat^)
goto :ok

:test
set "BIN=%GOH_BIN%"
if not "%~2"=="" set "BIN=%~2"
if "%BIN%"=="" set "BIN=F:\SteamLibrary\steamapps\common\Call to Arms - Gates of Hell\binaries\x64"
call :cc -DSF_TEST -o dist\sf_test.exe src\gem_squadfollow.c || goto :fail
>> "%LOG%" echo running on %BIN%
dist\sf_test.exe "%BIN%\call_to_arms.exe" "%BIN%\call_to_arms_ed.exe" "%BIN%\call_to_arms_server.exe" >> "%LOG%" 2>&1 || goto :fail
goto :ok

:inject
call :cc -s -municode -o dist\gem_inject.exe src\gem_inject.c || goto :fail
>> "%LOG%" echo built dist\gem_inject.exe
goto :ok

:cc
%CCX% %* >> "%LOG%" 2>&1 && exit /b 0
rem a stale zig global cache (objects deleted under %LocalAppData%\zig) fails like this; retry once
>> "%LOG%" echo compile failed, retrying with a local zig cache ^(.zig-cache^)
set "ZIG_GLOBAL_CACHE_DIR=%~dp0.zig-cache"
%CCX% %* >> "%LOG%" 2>&1
exit /b %errorlevel%

:ok
>> "%LOG%" echo OK
type "%LOG%"
if not defined NOPAUSE pause
exit /b 0

:fail
>> "%LOG%" echo FAILED
type "%LOG%"
if not defined NOPAUSE pause
exit /b 1
