#!/bin/sh
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build=$root/build/cov
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Debug -DFLASHFS_BUILD_TESTS=ON -DCMAKE_C_FLAGS="--coverage -O0" >/dev/null
cmake --build "$build" >/dev/null
find "$build" -name '*.gcda' -delete
ctest --test-dir "$build" >/dev/null
cd "$(find "$build" -name 'jffs2.c.gcda' -printf '%h\n' | head -n 1)"
gcov -n ./*.gcda 2>/dev/null | grep -A1 "^File '.*/src/" | grep -v '^--' | paste - - |
    sed -E "s|File '.*/src/([^']+)'.*executed:([0-9.]+)% of ([0-9]+)|\1 \2 \3|" |
    awk '{ printf "%-12s %6s%% of %5d lines\n", $1, $2, $3; c += $2 * $3 / 100; t += $3 }
         END { printf "total        %6.2f%% of %5d lines\n", 100 * c / t, t; exit (100 * c / t < 90) }'
