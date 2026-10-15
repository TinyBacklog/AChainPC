#!/bin/sh
# FM2 proof of concept: build and run the checks. Usage: run.sh OUT_DIR [--bench]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$1; shift
mkdir -p "$out"
CC=${CC:-clang}
CFLAGS="-O2 -march=x86-64-v3 -std=gnu11 -Wall -Wextra -Werror -I$here"
$CC $CFLAGS -o "$out/fm2_test" "$here/fm2_test.c" "$here/fm2_runtime.c" -lpthread
"$out/fm2_test" "$@"
python3 "$here/gen_sites.py" "$out/gen" "${FM2_SITES:-200000}" 8
$CC $CFLAGS -O1 -o "$out/fm2_sites" "$here/fm2_sites.c" "$here/fm2_runtime.c" "$out"/gen/*.c
"$out/fm2_sites"
