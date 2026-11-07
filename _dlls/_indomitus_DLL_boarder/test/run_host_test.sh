#!/bin/sh
# Linux offline check: test/run_host_test.sh <call_to_arms.exe> <call_to_arms_ed.exe> <call_to_arms_server.exe>
# Same BF_TEST code as `build.bat test`, same flags as the dll build. The stubs are executed against the
# mapped exe image (real engine prologues + crew check), so this needs x86-64.
set -e
d=$(dirname "$0")
cc=${CC:-clang}
$cc -O2 -mno-avx -ffp-contract=off -fno-fast-math -Wall -Wextra -DBF_TEST -I"$d/stub" -o "$d/bf_host_test" \
    "$d/../src/gem_boardfilter.c" "$d/host_map.c"
"$d/bf_host_test" "$@"
