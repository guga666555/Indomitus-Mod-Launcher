#!/bin/sh
# Host-side checks (Linux/WSL, gcc + clang):
#  1. ps_test maps each exe at its image base and runs every finder - run this after a game update.
#     call_to_arms.exe must find both; the editor has no unit preview (scene loads only).
#  2. clang syntax check of the real Windows build using fake headers in tests/stub.
set -e
cd "$(dirname "$0")/.."
mkdir -p out
gcc -O1 -DPS_TEST -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -o out/ps_test src/gem_previewscene.c
for exe in "$@"; do ./out/ps_test "$exe" || exit 1; done
CLANG_INC=$(clang -print-resource-dir)/include
clang -fsyntax-only -target x86_64-windows-gnu -fshort-wchar -nostdinc -I tests/stub -I "$CLANG_INC" -I /usr/include -I /usr/include/x86_64-linux-gnu \
      -Wall -Wextra -Wno-unused-parameter -Wno-void-pointer-to-int-cast src/gem_previewscene.c && echo "windows-target syntax OK"
