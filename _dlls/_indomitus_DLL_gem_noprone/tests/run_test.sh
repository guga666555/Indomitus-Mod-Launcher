#!/bin/sh
# Offline finder check on Linux/WSL: ./tests/run_test.sh <call_to_arms.exe> [<call_to_arms_ed.exe> ...]
# Maps each exe at its preferred base, runs every finder, repeats with a BreedTags-style hook on
# ApplyBreed, and checks that a hooked CanTakePose is refused. Each exe must end with ALL FOUND.
set -e
here=$(dirname "$0")
cc=${CC:-cc}
$cc -DNP_TEST -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -o "$here/np_test" "$here/../source/gem_noprone.c"
rc=0
for exe in "$@"; do echo "== $exe"; "$here/np_test" "$exe" || rc=1; done
exit $rc
