@echo off
rem Builds dist\gem_breedtags.dll (x86-64 Windows) with zig cc (mingw-w64 target).
rem zig: zig\zig.exe next to this file, the ScriptExt project's portable zig, zig on PATH, or "python -m ziglang".
setlocal
cd /d "%~dp0"
set ZIG=
if exist "zig\zig.exe" set ZIG=zig\zig.exe
if "%ZIG%"=="" if exist "..\_indomitus_DLL_scriptext\zig\zig.exe" set ZIG=..\_indomitus_DLL_scriptext\zig\zig.exe
if "%ZIG%"=="" where zig >nul 2>nul && set ZIG=zig
if "%ZIG%"=="" python -m ziglang version >nul 2>nul && set ZIG=python -m ziglang
if "%ZIG%"=="" (
    echo zig not found: copy a portable zig\ folder here, put zig on PATH, or pip install ziglang
    exit /b 1
)
if not exist dist mkdir dist
%ZIG% cc -target x86_64-windows-gnu -O2 -s -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers ^
    -o dist\gem_breedtags.dll source\gem_breedtags.c -lkernel32
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
copy /y gem_breedtags.ini dist\ >nul
echo built dist\gem_breedtags.dll
echo   via launcher:     copy dist\gem_breedtags.dll + gem_breedtags.ini into launcher\payload\ and rebuild the launcher
echo   as dbghelp proxy: copy dist\gem_breedtags.dll to binaries\x64\dbghelp.dll (+ gem_breedtags.ini)
endlocal
