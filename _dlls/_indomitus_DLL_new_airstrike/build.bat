@echo off
setlocal
rem GEM BombBay - Indomitus Launcher payload DLL
rem   build.bat            -> dist\gem_bombbay.dll
rem   build.bat test       -> dist\bb_test.exe   (offline finder check: bb_test.exe "<path>\call_to_arms.exe")
rem   build.bat payload    -> build, then copy dll + ini into ..\..\launcher\payload\  (rebuild the launcher after)
rem   build.bat clean
rem Compiler: the launcher repo's portable zig (..\..\zig\zig.exe), else zig on PATH, else python -m ziglang.

cd /d "%~dp0"
if not exist dist mkdir dist

set ZIG=
if exist "..\..\zig\zig.exe" set ZIG="..\..\zig\zig.exe"
if "%ZIG%"=="" for /d %%D in ("..\..\zig\zig-windows-x86_64-*") do if exist "%%~D\zig.exe" set ZIG="%%~D\zig.exe"
if "%ZIG%"=="" where zig >nul 2>nul && set ZIG=zig
if "%ZIG%"=="" python -m ziglang version >nul 2>nul && set ZIG=python -m ziglang
if "%ZIG%"=="" (
    echo No zig found: expected ..\..\zig\zig.exe ^(launcher repo^), zig on PATH, or "pip install ziglang".
    exit /b 1
)
echo using %ZIG%

set CFLAGS=-target x86_64-windows-gnu -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

if /i "%1"=="clean" ( del /q dist\*.dll dist\*.exe dist\*.pdb 2>nul & exit /b 0 )

if /i "%1"=="test" (
    %ZIG% cc %CFLAGS% -DBB_TEST "-Wl,--image-base=0x10000000" -o dist\bb_test.exe source\gem_bombbay.c -lkernel32 || exit /b 1
    echo built dist\bb_test.exe  -  run:  dist\bb_test.exe "F:\SteamLibrary\steamapps\common\Call to Arms - Gates of Hell\binaries\x64\call_to_arms.exe"
    exit /b 0
)

%ZIG% cc %CFLAGS% -s -shared -o dist\gem_bombbay.dll source\gem_bombbay.c -lkernel32 || exit /b 1
echo built dist\gem_bombbay.dll

if /i "%1"=="payload" (
    set PAYLOAD=..\..\launcher\payload
    if not exist "%PAYLOAD%" ( echo payload folder %PAYLOAD% not found & exit /b 1 )
    copy /y dist\gem_bombbay.dll "%PAYLOAD%\" >nul || exit /b 1
    copy /y gem_bombbay.ini "%PAYLOAD%\" >nul || exit /b 1
    echo copied gem_bombbay.dll + gem_bombbay.ini into %PAYLOAD%  -  now rebuild the launcher ^(build_zig.bat^)
)
exit /b 0
