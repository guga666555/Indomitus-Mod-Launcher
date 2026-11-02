@echo off
rem Builds GEM WeaponScript (gem_weaponscript.dll: the converge / overcharge / guidance modules in one dll) for the
rem Indomitus Launcher payload with zig cc (clang + mingw-w64, no Visual Studio).
rem   build.bat           -> out\gem_weaponscript.dll
rem   build.bat payload   -> same, then copies gem_weaponscript.dll + gem_weaponscript.ini into launcher\payload\
rem                          (then run build_zig.bat in the launcher folder: the payload is sealed into the exe)
rem   build.bat test      -> out\chain_test.exe   (run: out\chain_test.exe ...\binaries\x64\call_to_arms.exe, must end with ALL OK)
rem zig: the launcher's portable zig (..\..\zig\zig.exe), zig on PATH, or "python -m ziglang".
setlocal
cd /d "%~dp0"
set "ROOT=%~dp0..\.."
set ZIG=
if exist "%ROOT%\zig\zig.exe" set ZIG="%ROOT%\zig\zig.exe"
if not defined ZIG where zig >nul 2>nul && set ZIG=zig
if not defined ZIG python -m ziglang version >nul 2>nul && set ZIG=python -m ziglang
if not defined ZIG (
    echo zig not found: expected %ROOT%\zig\zig.exe, zig on PATH, or pip install ziglang
    exit /b 1
)
if not exist out mkdir out
set CC=%ZIG% cc -target x86_64-windows-gnu -O2 -s -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
set MODULES=gem_converge\gem_converge.c gem_overcharge\gem_overcharge.c gem_guidance\gem_guidance.c

if /i "%1"=="test" (
    %CC% -DGEM_TEST -Wl,--image-base=0x10000000 -o out\chain_test.exe test\chain_test.c %MODULES% || goto :fail
    echo built out\chain_test.exe - run it on call_to_arms.exe, call_to_arms_ed.exe and call_to_arms_server.exe
    goto :eof
)
%CC% -shared -o out\gem_weaponscript.dll gem_weaponscript\gem_weaponscript.c %MODULES% -lkernel32 || goto :fail
del out\gem_weaponscript.lib >nul 2>&1
echo built out\gem_weaponscript.dll
if /i "%1"=="payload" (
    if not exist "%ROOT%\launcher\payload\" (
        echo launcher\payload\ not found at %ROOT%\launcher\payload\
        goto :fail
    )
    copy /y out\gem_weaponscript.dll "%ROOT%\launcher\payload\" >nul || goto :fail
    copy /y gem_weaponscript\gem_weaponscript.ini "%ROOT%\launcher\payload\" >nul || goto :fail
    echo copied gem_weaponscript.dll + gem_weaponscript.ini into launcher\payload\ - now run build_zig.bat in the launcher folder
    echo   ^(if gem_converge / gem_overcharge / gem_guidance .dll or .ini are still in launcher\payload\, delete them^)
) else (
    echo   next: build.bat payload, then build_zig.bat in the launcher folder
)
goto :eof

:fail
echo BUILD FAILED
exit /b 1
