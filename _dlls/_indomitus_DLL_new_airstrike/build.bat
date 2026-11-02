@echo off
rem GEM BombBay - Indomitus Launcher payload DLL
rem   build.bat            -> dist\gem_bombbay.dll
rem   build.bat test       -> dist\bb_test.exe   (offline finder check: bb_test.exe "<path>\call_to_arms.exe")
rem   build.bat payload    -> build, then copy dll + ini into ..\..\launcher\payload\  (rebuild the launcher after)
rem   build.bat clean
rem Compiler: the launcher repo's portable zig (anywhere under ..\..\zig\), else zig on PATH, else python -m ziglang.
rem Everything printed also goes to build_log.txt next to this file; the window stays open at the end.

if not "%BB_LOGGED%"=="1" (
    set BB_LOGGED=1
    call "%~f0" %* > "%~dp0build_log.txt" 2>&1
    type "%~dp0build_log.txt"
    echo.
    echo [log written to build_log.txt]
    pause
    exit /b
)

setlocal
cd /d "%~dp0"
echo cwd: %CD%
if not exist dist mkdir dist

set ZIG=
if exist "..\..\zig\zig.exe" set ZIG="..\..\zig\zig.exe"
if "%ZIG%"=="" if exist "..\..\zig\" for /r "..\..\zig" %%F in (zig.exe) do if "%ZIG%"=="" set ZIG="%%F"
if "%ZIG%"=="" where zig >nul 2>nul && set ZIG=zig
if "%ZIG%"=="" python -m ziglang version >nul 2>nul && set ZIG=python -m ziglang
if "%ZIG%"=="" (
    echo ERROR: no zig found. Looked for zig.exe under ..\..\zig\ ^(launcher repo^), zig on PATH, and "python -m ziglang".
    dir /b /s "..\..\zig" 2>nul | findstr /i "zig.exe"
    exit /b 1
)
echo using %ZIG%
%ZIG% version

set CFLAGS=-target x86_64-windows-gnu -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

if /i "%1"=="clean" ( del /q dist\*.dll dist\*.exe dist\*.pdb 2>nul & echo cleaned & exit /b 0 )

if /i "%1"=="test" (
    echo compiling dist\bb_test.exe ...
    %ZIG% cc %CFLAGS% -DBB_TEST "-Wl,--image-base=0x10000000" -o dist\bb_test.exe source\gem_bombbay.c -lkernel32
    if errorlevel 1 (
        echo first attempt failed - retrying with a fresh zig cache in %~dp0.zig-cache
        set ZIG_GLOBAL_CACHE_DIR=%~dp0.zig-cache
        %ZIG% cc %CFLAGS% -DBB_TEST "-Wl,--image-base=0x10000000" -o dist\bb_test.exe source\gem_bombbay.c -lkernel32
        if errorlevel 1 ( echo ERROR: compile failed & exit /b 1 )
    )
    echo built dist\bb_test.exe  -  run:  dist\bb_test.exe "F:\SteamLibrary\steamapps\common\Call to Arms - Gates of Hell\binaries\x64\call_to_arms.exe"
    exit /b 0
)

echo compiling dist\gem_bombbay.dll ...
%ZIG% cc %CFLAGS% -s -shared -o dist\gem_bombbay.dll source\gem_bombbay.c -lkernel32
if errorlevel 1 (
    echo first attempt failed - retrying with a fresh zig cache in %~dp0.zig-cache ^(zig's global cache in %%LocalAppData%%\zig may be corrupt: delete it^)
    set ZIG_GLOBAL_CACHE_DIR=%~dp0.zig-cache
    %ZIG% cc %CFLAGS% -s -shared -o dist\gem_bombbay.dll source\gem_bombbay.c -lkernel32
    if errorlevel 1 ( echo ERROR: compile failed & exit /b 1 )
)
echo built dist\gem_bombbay.dll

if /i "%1"=="payload" (
    if not exist "..\..\launcher\payload" ( echo ERROR: payload folder ..\..\launcher\payload not found & exit /b 1 )
    copy /y dist\gem_bombbay.dll "..\..\launcher\payload\" >nul
    copy /y gem_bombbay.ini "..\..\launcher\payload\" >nul
    echo copied gem_bombbay.dll + gem_bombbay.ini into ..\..\launcher\payload  -  now rebuild the launcher ^(build_zig.bat^)
)
echo OK
exit /b 0
