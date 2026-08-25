#!/bin/bash
# Run `perf stat` across a probes sweep and aggregate IPC / cache-miss rates.
# Wraps ./profile.sh --stat for each probes value.
#
# Usage:
#   ./stat_sweep.sh [DURATION_SECONDS] [PROBES ...]
#   ./stat_sweep.sh 20 1 5 10 20 50 100
#
# Outputs: results/profile/stat_sweep.csv + per-probes logs (sweep_p<probes>.log)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

DURATION="${1:-${DURATION:-20}}"
shift 2>/dev/null || true
if [ $# -gt 0 ]; then
    PROBES_LIST="$*"
else
    PROBES_LIST="${STAT_PROBES:-1 5 10 20 50 100}"
fi

OUTDIR="$BENCH_DIR/results/profile"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/stat_sweep.csv"
: > "$SUMMARY"

log "stat sweep: probes=[$PROBES_LIST] duration=${DURATION}s"

for p in $PROBES_LIST; do
    LOG="$OUTDIR/sweep_p${p}.log"
    if ! bash "$(dirname "${BASH_SOURCE[0]}")/profile.sh" --stat "$p" "$DURATION" > "$LOG" 2>&1; then
        log "probes=$p FAILED (see $LOG)"
        echo "$p,,,,failed" >> "$SUMMARY"
        continue
    fi
    ipc=$(awk '/IPC \(insn\/cycle\)/ {print $NF}' "$LOG")
    llc=$(awk '/LLC cache miss rate/ {gsub(/%/, "", $NF); print $NF}' "$LOG")
    bm=$(awk '/branch miss rate/ {gsub(/%/, "", $NF); print $NF}' "$LOG")
    l1=$(awk '/L1 dcache miss rate/ {gsub(/%/, "", $NF); print $NF}' "$LOG")
    printf '  probes=%-4s IPC=%s LLCmiss=%s%% brmiss=%s%% L1miss=%s%%\n' \
        "$p" "${ipc:-?}" "${llc:-?}" "${bm:-?}" "${l1:-?}"
    echo "$p,$ipc,$llc,$bm,$l1" >> "$SUMMARY"
done

echo ""
echo "=== summary table (also in $SUMMARY) ==="
awk -F, 'BEGIN { printf "%-8s %-8s %-10s %-10s %-10s\n", "probes", "IPC", "LLCmiss%", "brmiss%", "L1miss%" }
         { printf "%-8s %-8s %-10s %-10s %-10s\n", $1, $2, $3, $4, $5 }' "$SUMMARY"
log "done"
