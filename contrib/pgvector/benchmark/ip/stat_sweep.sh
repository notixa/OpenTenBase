#!/bin/bash
# Run `perf stat` across a probes sweep and aggregate IPC / cache-miss rates.
# Wraps ./profile.sh --stat for each probes value.
#
# Usage:
#   ./stat_sweep.sh [DURATION_SECONDS] [PROBES ...]
#   ./stat_sweep.sh 10 1 5 10 20 50 100
#
# Outputs: results/stat_sweep.csv (aggregated, header + one row per probes)
#          + per-probes logs under results/profile/ (sweep_p<probes>.log)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

DURATION="${1:-${STAT_DURATION:-10}}"
shift 2>/dev/null || true
if [ $# -gt 0 ]; then
    PROBES_LIST="$*"
else
    PROBES_LIST="${STAT_PROBES:-1 5 10 20 50 100}"
fi

OUTDIR="$RESDIR/profile"
mkdir -p "$OUTDIR"
SUMMARY="$RESDIR/stat_sweep.csv"
echo "probes,IPC,LLCmiss%,brmiss%,L1miss%" > "$SUMMARY"

log "stat sweep: probes=[$PROBES_LIST] duration=${DURATION}s -> $SUMMARY"

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
awk -F, 'NR==1 { printf "%-8s %-8s %-10s %-10s %-10s\n", $1, $2, $3, $4, $5; next }
         { printf "%-8s %-8s %-10s %-10s %-10s\n", $1, $2, $3, $4, $5 }' "$SUMMARY"
log "done"
