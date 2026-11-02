#!/bin/sh
# Host-side checks (Linux/WSL, gcc + clang):
#  1. sx_test maps each exe at its image base and runs every finder - run this after a game update.
#  2. mult_stub_test runs the {multiplier} call stub against fake game/editor parser frames.
#  3. clang check of the real Windows build (x86_64-windows, LLP64) using fake headers in tests/stub, with the
#     build.bat warnings turned into errors.
# usage: sh tests/run_offline_check.sh <call_to_arms.exe> [call_to_arms_ed.exe] [call_to_arms_server.exe]
set -e
cd "$(dirname "$0")/.."
mkdir -p out
gcc -O1 -DSX_TEST -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -o out/sx_test src/gem_scriptext.c
for exe in "$@"; do ./out/sx_test "$exe" || exit 1; done
gcc -O2 -Wall -Wno-unused-function -o out/mult_stub_test tests/mult_stub_test.c && ./out/mult_stub_test
CLANG_INC=$(clang -print-resource-dir)/include
clang -fsyntax-only -target x86_64-windows-gnu -fshort-wchar -nostdinc -I tests/stub -I "$CLANG_INC" -I /usr/include -I /usr/include/x86_64-linux-gnu \
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Werror src/gem_scriptext.c && echo "windows-target check OK (no warnings)"
