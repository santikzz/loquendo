#!/bin/sh
# Build jorge on Linux, Raspberry Pi or macOS.
set -e
cd "$(dirname "$0")"

CC=${CC:-cc}
command -v "$CC" >/dev/null || { echo "$CC not found. Install gcc or clang."; exit 1; }

[ -f gen/funcs.h ] || { echo "gen/ is missing. Check out the full repository."; exit 1; }

echo "Compiling, this takes a few minutes..."
"$CC" -std=c11 -O1 -w -Iruntime -Igen gen/*.c runtime/*.c -o jorge -lm -lpthread
echo "Built ./jorge"
