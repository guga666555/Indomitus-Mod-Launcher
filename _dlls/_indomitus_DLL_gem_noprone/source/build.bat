@echo off
rem Builds GEM NoProne with zig cc (clang + mingw-w64).
rem   build.bat           -> out\gem_noprone.dll
rem   build.bat payload   -> same, then copies gem_noprone.dll + gem_noprone.ini into launcher\payload\
rem   build.bat test      -> out\np_test.exe   (np_test.exe ...\call_to_arms.exe must end with ALL FOUND)
rem zig: the launcher's portable zig (..\..\..\zig\zig.exe), zig on PATH, or "python -m ziglang".
setlocal
cd /d "%~dp0"
set "ROOT=%~dp0..\..\.."
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

if /i "%1"=="test" (
    %CC% -DNP_TEST -Wl,--image-base=0x10000000 -o out\np_test.exe gem_noprone.c || goto :fail
    echo built out\np_test.exe
    goto :eof
)
%CC% -shared -o out\gem_noprone.dll gem_noprone.c -lkernel32 || goto :fail
del out\gem_noprone.lib >nul 2>&1
echo built out\gem_noprone.dll
if /i "%1"=="payload" (
    copy /y out\gem_noprone.dll "%ROOT%\launcher\payload\" >nul || goto :fail
    copy /y ..\gem_noprone.ini "%ROOT%\launcher\payload\" >nul || goto :fail
    echo copied gem_noprone.dll + gem_noprone.ini into launcher\payload\ - now run build_zig.bat
) else (
    echo   next: build.bat payload, then build_zig.bat in the launcher folder
)
goto :eof

:fail
echo BUILD FAILED
exit /b 1
