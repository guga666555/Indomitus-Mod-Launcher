#!/bin/sh
# Host-side checks (Linux/WSL, gcc + clang). Usage: tests/run_offline_check.sh <call_to_arms.exe> [<editor.exe> <server.exe>]
#  1. rp_test maps each exe at its image base and runs every finder - run this after a game update.
#     call_to_arms.exe must print ALL FOUND (also with 6 vtable slots pre-hooked, as GEM ConquestUI does);
#     the editor and the dedicated server have no campaign UI (INERT).
#  2. rp_test --logic: fake research tree -> prerequisite path, highlight/redraw, tooltip text, string frees.
#  3. clang syntax check of the real Windows build using fake headers in tests/stub.
set -e
cd "$(dirname "$0")/.."
mkdir -p out
gcc -O1 -DRP_TEST -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -o out/rp_test src/gem_researchpath.c
./out/rp_test --logic
for exe in "$@"; do ./out/rp_test "$exe" || exit 1; done
CLANG_INC=$(clang -print-resource-dir)/include
clang -fsyntax-only -target x86_64-windows-gnu -fshort-wchar -nostdinc -I tests/stub -I "$CLANG_INC" -I /usr/include -I /usr/include/x86_64-linux-gnu \
      -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers src/gem_researchpath.c && echo "windows-target syntax OK"
