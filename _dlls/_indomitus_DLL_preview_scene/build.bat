@echo off
rem Builds dist\gem_previewscene.dll (x86-64 Windows) with zig cc (mingw-w64 target).
rem zig: portable copy in .\zig\ or in the launcher project (..\..\zig\zig.exe), zig on PATH, or "python -m ziglang".
setlocal
cd /d "%~dp0"
set ZIG=
if exist "zig\zig.exe" set ZIG="%~dp0zig\zig.exe"
if not defined ZIG if exist "..\..\zig\zig.exe" set ZIG="%~dp0..\..\zig\zig.exe"
if not defined ZIG if exist "..\_indomitus_DLL_scriptext\zig\zig.exe" set ZIG="%~dp0..\_indomitus_DLL_scriptext\zig\zig.exe"
if not defined ZIG where zig >nul 2>nul && set ZIG=zig
if not defined ZIG python -m ziglang version >nul 2>nul && set ZIG=python -m ziglang
if not defined ZIG (
    echo zig not found: copy the zig\ folder of the launcher project here, put zig on PATH, or pip install ziglang
    exit /b 1
)
echo using %ZIG%
if not exist dist mkdir dist
%ZIG% cc -target x86_64-windows-gnu -O2 -s -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers ^
    -o dist\gem_previewscene.dll src\gem_previewscene.c -lkernel32 -luser32
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
del dist\*.lib >nul 2>nul
copy /y gem_previewscene.ini dist\ >nul
echo built dist\gem_previewscene.dll
echo   install:  copy dist\gem_previewscene.dll + gem_previewscene.ini into ..\..\launcher\payload\ and rebuild the launcher
endlocal
