@echo off
rem GEM SupplyTags build - Indomitus Launcher payload dll (x86-64 Windows, zig cc with the mingw-w64 target)
rem   build.bat           -> out\gem_supplytags.dll + out\gem_supplytags.ini
rem   build.bat payload   -> the above, then copies both into ..\..\launcher\payload\ (then run build_zig.bat there)
rem   build.bat test      -> out\sp_test.exe: the finders on the three game exes (one exe per process) + the hold logic.
rem                          Game folder: GOH_BIN (binaries\x64), default the Steam folder on C:
rem zig: .\zig\zig.exe if there is one, else the launcher's ..\..\zig\zig.exe (build_zig.bat downloads it),
rem      else the zig.exe named by ZIG_EXE, else zig on PATH.
setlocal
cd /d "%~dp0"
set ZIG=
if exist "zig\zig.exe" set ZIG="%~dp0zig\zig.exe"
if not defined ZIG if exist "..\..\zig\zig.exe" set ZIG="%~dp0..\..\zig\zig.exe"
if not defined ZIG if defined ZIG_EXE if exist "%ZIG_EXE%" set ZIG="%ZIG_EXE%"
if not defined ZIG where zig >nul 2>nul && set ZIG=zig
if not defined ZIG (
    echo zig not found: run build_zig.bat in the launcher folder once ^(it downloads zig^), set ZIG_EXE to a zig.exe, or put zig on PATH
    exit /b 1
)
echo using %ZIG%
if not exist out mkdir out
set CC=%ZIG% cc -target x86_64-windows-gnu -mcpu=baseline -O2 -Wall -Wextra
if /i "%~1"=="test" goto :test

%CC% -shared -s -o out\gem_supplytags.dll gem_supplytags.c
if errorlevel 1 goto :failed
del out\*.lib >nul 2>nul
copy /y gem_supplytags.ini out\ >nul
if errorlevel 1 goto :failed
if exist inject.bat copy /y inject.bat out\ >nul
if exist ..\gem_meleestance\out\gem_inject.exe copy /y ..\gem_meleestance\out\gem_inject.exe out\ >nul
echo built out\gem_supplytags.dll
if /i "%~1"=="payload" goto :payload
echo   next: build.bat payload, then build_zig.bat in the launcher folder
exit /b 0

:payload
if not exist "..\..\launcher\payload\" (
    echo launcher\payload\ not found - this folder is expected at _dlls\^<name^>\ inside the launcher repository
    exit /b 1
)
copy /y out\gem_supplytags.dll "..\..\launcher\payload\" >nul || exit /b 1
copy /y out\gem_supplytags.ini "..\..\launcher\payload\" >nul || exit /b 1
echo copied gem_supplytags.dll + gem_supplytags.ini into launcher\payload\ - now run build_zig.bat in the launcher folder
exit /b 0

:test
if not defined GOH_BIN set "GOH_BIN=C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell\binaries\x64"
%CC% -DSP_TEST -o out\sp_test.exe gem_supplytags.c
if errorlevel 1 goto :failed
out\sp_test.exe "%GOH_BIN%\call_to_arms.exe" || exit /b 1
out\sp_test.exe "%GOH_BIN%\call_to_arms_ed.exe" || exit /b 1
out\sp_test.exe "%GOH_BIN%\call_to_arms_server.exe" || exit /b 1
exit /b 0

:failed
echo BUILD FAILED
exit /b 1
