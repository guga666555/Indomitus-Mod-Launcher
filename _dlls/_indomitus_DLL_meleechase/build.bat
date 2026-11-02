@echo off
rem Builds dist\gem_meleechase.dll (x86-64 Windows) with zig cc (mingw-w64 target).
rem zig: portable copy in .\zig\, or the ScriptExt / launcher copy, or zig on PATH, or "python -m ziglang".
setlocal
cd /d "%~dp0"
set ZIG=
if exist "zig\zig.exe" set ZIG=zig\zig.exe
if "%ZIG%"=="" if exist "..\_indomitus_DLL_scriptext\zig\zig.exe" set ZIG=..\_indomitus_DLL_scriptext\zig\zig.exe
if "%ZIG%"=="" if exist "..\..\zig\zig.exe" set ZIG=..\..\zig\zig.exe
if "%ZIG%"=="" where zig >nul 2>nul && set ZIG=zig
if "%ZIG%"=="" python -m ziglang version >nul 2>nul && set ZIG=python -m ziglang
if "%ZIG%"=="" (
    echo zig not found: copy the ScriptExt zig\ folder here, put zig on PATH, or pip install ziglang
    exit /b 1
)
if not exist dist mkdir dist
%ZIG% cc -target x86_64-windows-gnu -O2 -s -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers ^
    -o dist\gem_meleechase.dll src\gem_meleechase.c -lkernel32 -luser32
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
copy /y gem_meleechase.ini dist\ >nul
echo built dist\gem_meleechase.dll
echo   via launcher: copy dist\gem_meleechase.dll + gem_meleechase.ini into launcher\payload\ and rebuild the launcher
endlocal
