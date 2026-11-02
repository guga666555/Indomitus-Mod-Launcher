@echo off
rem Builds the three GEM hook dlls with zig's bundled clang + mingw-w64 (pip install ziglang).
rem   build.bat        -> out\gem_converge.dll, out\gem_overcharge.dll, out\gem_guidance.dll
rem   build.bat test   -> out\chain_test.exe   (run: out\chain_test.exe ...\binaries\x64\call_to_arms.exe)
setlocal
cd /d "%~dp0"
if not exist out mkdir out
set CC=python -m ziglang cc -target x86_64-windows-gnu -O2 -s -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
if "%1"=="test" (
  %CC% -DGEM_TEST -Wl,--image-base=0x10000000 -o out\chain_test.exe test\chain_test.c gem_converge\gem_converge.c gem_overcharge\gem_overcharge.c gem_guidance\gem_guidance.c || exit /b 1
  goto :eof
)
for %%d in (gem_converge gem_overcharge gem_guidance) do (
  %CC% -shared -o out\%%d.dll %%d\%%d.c -lkernel32 || exit /b 1
)
