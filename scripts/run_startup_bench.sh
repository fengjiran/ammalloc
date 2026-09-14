#!/usr/bin/env bash
# Runs each startup/degradation/stress mode in its own fresh process so cold-init
# timing and fault injection never share an address space. Results are appended
# to a CSV for cross-run comparison.
#
# usage: scripts/run_startup_bench.sh [binary] [repeats] [out_csv]
set -euo pipefail

BIN="${1:-build/tests/benchmark/startup/ammalloc_bench_startup}"
REPEATS="${2:-10}"
OUT="${3:-bench-startup-$(date +%Y%m%d).csv}"

if [[ ! -x "$BIN" ]]; then
    echo "error: benchmark binary not found or not executable: $BIN" >&2
    echo "       build it with: cmake --build build --target ammalloc_bench_startup" >&2
    exit 1
fi

echo "mode,run,output" > "$OUT"

run_mode() {
    local mode="$1"; shift
    for ((r = 1; r <= REPEATS; ++r)); do
        # Each mode gets a fresh process; capture the single summary line.
        local line
        line="$("$BIN" --mode="$mode" "$@" 2>/dev/null | tail -1)"
        # Quote the line so commas inside it stay in one CSV field.
        echo "${mode},${r},\"${line}\"" >> "$OUT"
    done
}

# cold_init must run first in a pristine process; the rest are independent.
run_mode cold_init
run_mode reset_latency --iters=2000
run_mode degraded_no_tc --iters=20000
run_mode degraded_partial_fetch --iters=20000 --cap=1
run_mode degraded_partial_fetch --iters=20000 --cap=64
run_mode degraded_zero_return --iters=20000
# steady_stress is long-running; use fewer repeats.
run_mode steady_stress --seconds=10

echo "wrote $OUT"
