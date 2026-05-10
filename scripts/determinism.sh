#!/bin/sh
set -eu
dump=$1
shift
n=0
bad=0
for arg in "$@"; do
    for img in $(find "$arg" -maxdepth 1 -name '*.img' | sort); do
        a=$("$dump" "$img" 2>&1 || true)
        b=$("$dump" "$img" 2>&1 || true)
        c=$("$dump" -c "$img" 2>&1 || true)
        d=$("$dump" -c "$img" 2>&1 || true)
        n=$((n + 1))
        if [ "$a" != "$b" ] || [ "$a" != "$c" ] || [ "$a" != "$d" ]; then
            echo "differs: $img"
            bad=$((bad + 1))
        fi
    done
done
echo "determinism: $n images, $bad differ"
[ "$bad" -eq 0 ] && [ "$n" -gt 0 ]
