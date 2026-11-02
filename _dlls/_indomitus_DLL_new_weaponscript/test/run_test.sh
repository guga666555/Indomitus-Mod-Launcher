#!/bin/sh
# Offline finder + hook-chain check on Linux/WSL: ./test/run_test.sh <call_to_arms.exe> [<call_to_arms_ed.exe> ...]
# Builds test/chain_test with the system C compiler (-DGEM_TEST: no Win32 API) and runs it on each exe.
# Each exe must end with ALL OK (both scenarios: clean exe, and entries pre-patched by another payload dll).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
$cc -DGEM_TEST -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -o "$here/test/chain_test" \
    "$here/test/chain_test.c" "$here/gem_converge/gem_converge.c" "$here/gem_overcharge/gem_overcharge.c" "$here/gem_guidance/gem_guidance.c" -lm
rc=0
for exe in "$@"; do echo "== $exe"; "$here/test/chain_test" "$exe" || rc=1; done
exit $rc
