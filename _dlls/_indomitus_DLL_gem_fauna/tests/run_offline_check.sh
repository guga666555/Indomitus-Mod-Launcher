#!/bin/sh
# Host-side checks (Linux/WSL, gcc + clang):
#  1. fx_test maps each exe at its image base and runs every finder - run this after a game update.
#  2. clang check of the real Windows build (x86_64-windows, LLP64) using fake headers in tests/stub, with the
#     build.bat warnings turned into errors.
# usage: sh tests/run_offline_check.sh <call_to_arms.exe> <call_to_arms_ed.exe> <call_to_arms_server.exe>
set -e
cd "$(dirname "$0")/.."
mkdir -p out
gcc -O1 -DFX_TEST -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -o out/fx_test src/gem_fauna.c
for exe in "$@"; do ./out/fx_test "$exe" || exit 1; done
CLANG_INC=$(clang -print-resource-dir)/include
clang -fsyntax-only -target x86_64-windows-gnu -fshort-wchar -nostdinc -I tests/stub -I "$CLANG_INC" -I /usr/include -I /usr/include/x86_64-linux-gnu \
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Werror src/gem_fauna.c && echo "windows-target check OK (no warnings)"
