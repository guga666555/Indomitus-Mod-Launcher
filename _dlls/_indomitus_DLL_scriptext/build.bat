@echo off
rem Builds dist\gem_scriptext.dll (x86-64 Windows) with zig cc (mingw-w64 target).
rem zig: portable copy in .\zig\ (copy it from the Indomitus Launcher project), or zig on PATH,
rem or "python -m ziglang" (pip install ziglang).
setlocal
cd /d "%~dp0"
set ZIG=
if exist "zig\zig.exe" set ZIG=zig\zig.exe
if "%ZIG%"=="" where zig >nul 2>nul && set ZIG=zig
if "%ZIG%"=="" python -m ziglang version >nul 2>nul && set ZIG=python -m ziglang
if "%ZIG%"=="" (
    echo zig not found: copy the launcher's zig\ folder here, put zig on PATH, or pip install ziglang
    exit /b 1
)
if not exist dist mkdir dist
%ZIG% cc -target x86_64-windows-gnu -O2 -s -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers ^
    -o dist\gem_scriptext.dll src\gem_scriptext.c -lkernel32 -luser32
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
copy /y gem_scriptext.ini dist\ >nul
echo built dist\gem_scriptext.dll
echo   as dbghelp proxy: copy dist\gem_scriptext.dll to binaries\x64\dbghelp.dll (+ gem_scriptext.ini)
echo   via launcher:     copy dist\gem_scriptext.dll + gem_scriptext.ini into launcher\payload\ and rebuild the launcher
endlocal
