#!/bin/sh
# Prints the little-endian instruction word for one AArch64 instruction.
# PC-relative branches take a byte offset from the instruction: "cbz w8, .+0x30".
# Needs clang and llvm-objcopy.
# Usage: game/patches/encode.sh "mov w1, #1"
set -eu
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
printf '.text\n%s\n' "$1" > "$dir/i.s"
clang --target=aarch64-linux-gnu -c "$dir/i.s" -o "$dir/i.o"
llvm-objcopy -O binary --only-section=.text "$dir/i.o" "$dir/i.bin"
od -An -tx4 -N4 "$dir/i.bin" | tr -d ' \n'
echo
