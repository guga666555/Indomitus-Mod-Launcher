#!/bin/sh
# Linux offline check: test/run_host_test.sh <call_to_arms.exe> <call_to_arms_ed.exe> <call_to_arms_server.exe>
# Same SF_TEST code as `build.bat test`, same float flags as the dll build.
set -e
d=$(dirname "$0")
cc=${CC:-clang}
$cc -O2 -mno-avx -ffp-contract=off -fno-fast-math -DSF_TEST -I"$d/stub" -o "$d/sf_host_test" \
    "$d/../src/gem_squadfollow.c" "$d/host_map.c" -lm
"$d/sf_host_test" "$@"
