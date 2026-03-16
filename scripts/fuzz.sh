#!/bin/sh
set -eu
secs=${1:-3600}
build=${2:-build/fuzz-ubsan}
root=$(cd "$(dirname "$0")/.." && pwd)
work=$build/fuzz-work
for fmt in jffs2 cramfs ubi; do
    mkdir -p "$work/$fmt/corpus" "$work/$fmt/crashes"
    cp "$root"/tests/data/*"$fmt"*.img "$work/$fmt/corpus/" 2>/dev/null || true
    for txt in "$root"/tests/parity/data/*.txt; do
        if head -n 1 "$txt" | grep -qx "format $fmt"; then
            cp "${txt%.txt}.img" "$work/$fmt/corpus/" 2>/dev/null || true
        fi
    done
done
for fmt in jffs2 cramfs ubi; do
    "$build/fuzz_$fmt" "$work/$fmt/corpus" -max_total_time="$secs" -max_len=1048576 -rss_limit_mb=2048 \
        -artifact_prefix="$work/$fmt/crashes/" -print_final_stats=1 >"$work/$fmt.log" 2>&1 &
done
status=0
for job in $(jobs -p); do
    wait "$job" || status=1
done
for fmt in jffs2 cramfs ubi; do
    printf '%s: ' "$fmt"
    grep -E "stat::number_of_executed_units|stat::new_units_added" "$work/$fmt.log" | tr '\n' ' '
    printf 'crashes: %s\n' "$(ls "$work/$fmt/crashes" | wc -l)"
done
exit $status
