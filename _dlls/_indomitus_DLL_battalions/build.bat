@echo off
setlocal
cd /d "%~dp0"

rem zig: the launcher project's portable zig (..\..\zig when this repo sits in _indomitus_mod_launcher\_dlls),
rem      else zig on PATH, else python -m ziglang
set ZIG=
if exist "..\..\zig\zig.exe" set ZIG=..\..\zig\zig.exe
if exist "zig\zig.exe" set ZIG=zig\zig.exe
if "%ZIG%"=="" ( where zig >nul 2>nul && set ZIG=zig )
if "%ZIG%"=="" ( python -m ziglang version >nul 2>nul && set ZIG=python -m ziglang )
if "%ZIG%"=="" (
    echo zig not found: copy the launcher's zig\ folder here or put zig on PATH
    exit /b 1
)

if not exist dist mkdir dist
%ZIG% cc -target x86_64-windows-gnu -std=c11 -O2 -Wall -Wextra -shared ^
    -o dist\gem_battalions.dll src\gem_battalions.c -lkernel32
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
copy /y gem_battalions.ini dist\ >nul
echo built dist\gem_battalions.dll + gem_battalions.ini
echo copy both into the launcher's launcher\payload\ and rebuild the launcher
endlocal
