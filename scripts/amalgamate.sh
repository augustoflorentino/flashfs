#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/dist/flashfs/flashfs.h}
mkdir -p "$(dirname "$out")"

{
    cat "$root/include/flashfs/flashfs.h"
    echo
    echo "#if defined(MTDF_IMPLEMENTATION) && !defined(MTDF_IMPLEMENTATION_DONE)"
    echo "#define MTDF_IMPLEMENTATION_DONE"
    for src in "$root"/src/*.h "$root"/src/*.c; do
        [ -f "$src" ] && grep -v '^#include "' "$src"
    done
    echo "#endif"
} > "$out"
echo "$out"
