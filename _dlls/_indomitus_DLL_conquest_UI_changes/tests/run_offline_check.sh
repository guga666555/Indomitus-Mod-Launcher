#!/bin/sh
# Host-side checks (Linux/WSL, gcc + clang):
#  1. cu_test maps each exe at its image base and runs every finder - run this after a game update.
#     call_to_arms.exe must print ALL FOUND; the editor and the dedicated server have no campaign UI (INERT).
#  2. logic_test simulates a panel / wrap panel / reveal queue and checks pf_apply (order, visibility, queue);
#     zoom_test simulates research tree icons, lines and a scroll pane and checks the zoom (geometry, anchor, limits).
#  3. clang syntax check of the real Windows build using fake headers in tests/stub.
set -e
cd "$(dirname "$0")/.."
mkdir -p out
gcc -O1 -DCU_TEST -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -o out/cu_test src/gem_conquestui.c
for exe in "$@"; do ./out/cu_test "$exe" || exit 1; done
gcc -O1 -Wall -Wno-unused-function -Wno-unused-variable -o out/logic_test tests/logic_test.c && ./out/logic_test
gcc -O1 -Wall -Wno-unused-function -Wno-unused-variable -o out/zoom_test tests/zoom_test.c -lm && ./out/zoom_test
CLANG_INC=$(clang -print-resource-dir)/include
clang -fsyntax-only -target x86_64-windows-gnu -fshort-wchar -nostdinc -I tests/stub -I "$CLANG_INC" -I /usr/include -I /usr/include/x86_64-linux-gnu \
      -Wall -Wextra -Wno-unused-parameter src/gem_conquestui.c && echo "windows-target syntax OK"
