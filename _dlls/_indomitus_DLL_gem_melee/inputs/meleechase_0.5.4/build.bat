@echo off
rem GEM MeleeChase build - Indomitus Launcher payload dll (x86-64 Windows, zig cc with the mingw-w64 target)
rem   build.bat          -> dist\gem_meleechase.dll + dist\gem_meleechase.ini
rem   build.bat payload  -> the above, then copies both into ..\..\launcher\payload\ (then run build_zig.bat there)
rem zig: .\zig\zig.exe if there is one, else the launcher's ..\..\zig\zig.exe (build_zig.bat downloads it),
rem      else the ScriptExt folder's, else zig on PATH, else "python -m ziglang" (pip install ziglang).
setlocal
cd /d "%~dp0"
set ZIG=
if exist "zig\zig.exe" set ZIG="%~dp0zig\zig.exe"
if not defined ZIG if exist "..\..\zig\zig.exe" set ZIG="%~dp0..\..\zig\zig.exe"
if not defined ZIG if exist "..\_indomitus_DLL_scriptext\zig\zig.exe" set ZIG="%~dp0..\_indomitus_DLL_scriptext\zig\zig.exe"
if not defined ZIG where zig >nul 2>nul && set ZIG=zig
if not defined ZIG python -m ziglang version >nul 2>nul && set ZIG=python -m ziglang
if not defined ZIG (
    echo zig not found: run build_zig.bat in the launcher folder once ^(it downloads zig^), put zig on PATH, or pip install ziglang
    exit /b 1
)
echo using %ZIG%
if not exist dist mkdir dist
%ZIG% cc -target x86_64-windows-gnu -O2 -s -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers ^
    -o dist\gem_meleechase.dll src\gem_meleechase.c -lkernel32 -luser32
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
del dist\*.lib >nul 2>nul
copy /y gem_meleechase.ini dist\ >nul
echo built dist\gem_meleechase.dll
if /i "%~1"=="payload" goto :payload
echo   next: build.bat payload, then build_zig.bat in the launcher folder
exit /b 0

:payload
if not exist "..\..\launcher\payload\" (
    echo launcher\payload\ not found - this folder is expected at _dlls\^<name^>\ inside the launcher repository
    exit /b 1
)
copy /y dist\gem_meleechase.dll "..\..\launcher\payload\" >nul || exit /b 1
copy /y dist\gem_meleechase.ini "..\..\launcher\payload\" >nul || exit /b 1
echo copied gem_meleechase.dll + gem_meleechase.ini into launcher\payload\ - now run build_zig.bat in the launcher folder
exit /b 0
