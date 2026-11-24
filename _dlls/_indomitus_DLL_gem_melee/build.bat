@echo off
rem GEM Melee build - Indomitus Launcher payload dll (x86-64 Windows, zig cc with the mingw-w64 target)
rem   build.bat            -> out\dll\gem_melee.dll + out\dll\gem_melee.ini   (zig only, no Python)
rem   build.bat payload    -> the above, then copies both into ..\..\launcher\payload\ (then run build_zig.bat there)
rem   build.bat <target>   -> any other word goes to tools\build.py as before (Alex's test targets: all, test,
rem                           parity, ...). Those need Python and the zig path written in tools\build.py.
rem The dll is built the way tools\build.py builds its "dll" target: same sources, same flags, same link order.
rem zig 0.14.0: .\zig\zig.exe if there is one, else the launcher's ..\..\zig\zig.exe (build_zig.bat downloads it),
rem      else the zig.exe named by ZIG_EXE, else zig on PATH.
setlocal
cd /d "%~dp0"
if "%~1"=="" goto :dll
if /i "%~1"=="dll" goto :dll
if /i "%~1"=="payload" goto :dll
python "%~dp0tools\build.py" %*
exit /b

:dll
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
set BASE=-target x86_64-windows-gnu -mcpu=baseline -O2 -ffp-contract=off -fno-fast-math
set WARN=-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
set OBJ=out\obj\dll
if not exist %OBJ% mkdir %OBJ%
if not exist out\dll mkdir out\dll

echo [dll] cc src\core\core.c
%ZIG% cc -c %BASE% %WARN% src\core\core.c -o %OBJ%\core.o
if errorlevel 1 goto :failed
echo [dll] cc src\core\core_patch.c
%ZIG% cc -c %BASE% %WARN% src\core\core_patch.c -o %OBJ%\core_patch.o
if errorlevel 1 goto :failed
echo [dll] cc src\core\core_crash.c
%ZIG% cc -c %BASE% %WARN% src\core\core_crash.c -o %OBJ%\core_crash.o
if errorlevel 1 goto :failed
echo [dll] cc src\core\core_dllmain.c
%ZIG% cc -c %BASE% %WARN% src\core\core_dllmain.c -o %OBJ%\core_dllmain.o
if errorlevel 1 goto :failed
echo [dll] cc src\link\link.c
%ZIG% cc -c %BASE% %WARN% src\link\link.c -o %OBJ%\link.o
if errorlevel 1 goto :failed
echo [dll] cc src\chase\chase.c
%ZIG% cc -c %BASE% %WARN% src\chase\chase.c -o %OBJ%\chase.o
if errorlevel 1 goto :failed
echo [dll] cc src\stance\stance.c
%ZIG% cc -c %BASE% src\stance\stance.c -o %OBJ%\stance.o
if errorlevel 1 goto :failed
echo [dll] cc src\pen\pen.c
%ZIG% cc -c %BASE% src\pen\pen.c -o %OBJ%\pen.o
if errorlevel 1 goto :failed

echo [dll] link out\dll\gem_melee.dll
%ZIG% cc -target x86_64-windows-gnu -mcpu=baseline -O2 -shared -s ^
    %OBJ%\core.o %OBJ%\core_patch.o %OBJ%\core_crash.o %OBJ%\core_dllmain.o %OBJ%\link.o %OBJ%\chase.o %OBJ%\stance.o %OBJ%\pen.o ^
    -lkernel32 -o out\dll\gem_melee.dll
if errorlevel 1 goto :failed
del out\dll\*.lib >nul 2>nul
copy /y src\gem_melee.ini out\dll\ >nul
if errorlevel 1 goto :failed
copy /y src\README.md out\dll\ >nul
echo built out\dll\gem_melee.dll
if /i "%~1"=="payload" goto :payload
echo   next: build.bat payload, then build_zig.bat in the launcher folder
exit /b 0

:payload
if not exist "..\..\launcher\payload\" (
    echo launcher\payload\ not found - this folder is expected at _dlls\^<name^>\ inside the launcher repository
    exit /b 1
)
copy /y out\dll\gem_melee.dll "..\..\launcher\payload\" >nul || exit /b 1
copy /y out\dll\gem_melee.ini "..\..\launcher\payload\" >nul || exit /b 1
echo copied gem_melee.dll + gem_melee.ini into launcher\payload\ - now run build_zig.bat in the launcher folder
echo   GEM Melee needs GEM Teleport 0.2.14 beside it, and no gem_meleestance / gem_meleechase / gem_penfilter dll
exit /b 0

:failed
echo BUILD FAILED
exit /b 1
