#!/bin/sh
# Builds GEM BreedTags for x86-64 Windows with zig's bundled clang + mingw-w64 (pip install ziglang).
set -e
CC="python3 -m ziglang cc -target x86_64-windows-gnu -O2 -s -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"
mkdir -p out
$CC -o out/gem_breedtags.dll gem_breedtags.c -lkernel32      # launcher payload name; rename to dbghelp.dll for the proxy install
